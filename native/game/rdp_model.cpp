// Per-frame drawing-time model: how long a real N64 would take to draw each
// display list the game submits, so the game can be paced the way the console
// paced it.
//
// Knife Edge steps only when no graphics task is pending. On hardware that
// ties its speed to the RSP and RDP: busy frames take longer to draw, so the
// game slows down exactly where the screen gets busy. RT64 draws instantly, so
// without this the game steps on every retrace. The CPU is not the limit -- the
// instruction counter puts a step at 3-8 ms of R4300 time -- so this estimates
// the graphics side, walking the list as F3DEX2 would:
//
//   RSP: each vertex is transformed and each triangle set up.
//   RDP: each triangle is set up, then its covered pixels are filled; the cost
//        per pixel depends on the cycle type (fill, copy, 1-cycle, 2-cycle) and
//        on whether the Z-buffer is read and written.
//
// Vertices are transformed with the matrices the game actually loaded, and each
// triangle's screen area is taken after the viewport transform. The RSP feeds
// the RDP through a FIFO, so the two overlap and a frame costs roughly the
// slower of them. The rates below are published N64 figures and are the
// model's assumptions; nothing here was measured on a console.

#include "rdp_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "recomp.h"

namespace {

// Clocks.
constexpr double rcp_hz = 62.5e6;         // RSP and RDP both run at 62.5 MHz

// RSP (F3DEX2), in RCP cycles.
constexpr double rsp_per_vertex = 70.0;   // transform, clip codes, lighting off
constexpr double rsp_per_triangle = 90.0; // setup and RDP command generation

// RDP, in RCP cycles.
constexpr double rdp_per_triangle = 30.0;       // edge walk setup
constexpr double rdp_per_pixel_1cycle = 1.0;
constexpr double rdp_per_pixel_2cycle = 2.0;
constexpr double rdp_zbuffer_factor = 1.6;      // Z read-modify-write memory traffic
constexpr double rdp_per_pixel_fill = 0.25;     // fill mode: four 16-bit pixels a cycle
constexpr double rdp_per_pixel_copy = 0.25;     // copy mode texrects
constexpr double rdp_per_command = 2.0;         // every command costs something

struct Matrix {
    std::array<float, 16> m{};
};

Matrix identity() {
    Matrix out;
    out.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return out;
}

Matrix multiply(const Matrix& a, const Matrix& b) {  // row vectors: v * a * b
    Matrix out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[r * 4 + k] * b.m[k * 4 + c];
            }
            out.m[r * 4 + c] = sum;
        }
    }
    return out;
}

uint32_t word(uint8_t* rdram, uint32_t phys) {
    return static_cast<uint32_t>(MEM_W(0, static_cast<gpr>(static_cast<int32_t>(0x80000000u | phys))));
}

int32_t half(uint8_t* rdram, uint32_t phys) {
    return MEM_H(0, static_cast<gpr>(static_cast<int32_t>(0x80000000u | phys)));
}

uint32_t uhalf(uint8_t* rdram, uint32_t phys) {
    return MEM_HU(0, static_cast<gpr>(static_cast<int32_t>(0x80000000u | phys)));
}

bool readable(uint32_t phys, uint32_t len) {
    return phys < 0x800000u && phys + len <= 0x800000u;
}

Matrix read_matrix(uint8_t* rdram, uint32_t phys) {
    Matrix out;
    for (int i = 0; i < 16; i++) {
        const uint32_t whole = uhalf(rdram, phys + i * 2);
        const uint32_t frac = uhalf(rdram, phys + 32 + i * 2);
        out.m[i] = static_cast<float>(static_cast<int32_t>((whole << 16) | frac)) / 65536.0f;
    }
    return out;
}

struct ScreenVertex {
    float cx = 0, cy = 0, cw = 1;  // clip space
    bool valid = false;
};

struct Point {
    double x, y;
};

// Sutherland-Hodgman against one edge; keep points where inside() is true.
template <typename Inside, typename Cross>
std::vector<Point> clip_edge(const std::vector<Point>& in, Inside inside, Cross cross) {
    std::vector<Point> out;
    for (size_t i = 0; i < in.size(); i++) {
        const Point& a = in[i];
        const Point& b = in[(i + 1) % in.size()];
        const bool ia = inside(a), ib = inside(b);
        if (ia) {
            out.push_back(a);
        }
        if (ia != ib) {
            out.push_back(cross(a, b));
        }
    }
    return out;
}

double polygon_area(const std::vector<Point>& poly) {
    double sum = 0;
    for (size_t i = 0; i < poly.size(); i++) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % poly.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum * 0.5;
}

double last_culled_fraction = 0;

}  // namespace

extern "C" double knife_edge_last_culled_fraction(void) {
    return last_culled_fraction;
}

extern "C" double knife_edge_estimate_frame_ms(uint8_t* rdram, uint32_t display_list) {
    uint32_t segment[16] = {};
    std::vector<uint32_t> returns;
    std::vector<Matrix> modelview_stack;
    Matrix projection = identity();
    Matrix modelview = identity();
    Matrix combined = identity();
    bool combined_dirty = true;
    std::array<ScreenVertex, 64> vertices{};
    // Viewport, in pixels: scale and translate. The game's own G_MOVEMEM sets it.
    float vp_scale_x = 160, vp_scale_y = 120, vp_trans_x = 160, vp_trans_y = 120;
    bool zbuffer = false;
    uint32_t cycle_type = 0;  // 0 1-cycle, 1 2-cycle, 2 copy, 3 fill

    double rsp_cycles = 0, rdp_cycles = 0;
    uint32_t pc = display_list & 0x00FFFFFFu;
    const auto resolve = [&](uint32_t address) {
        return (segment[(address >> 24) & 0xF] + (address & 0x00FFFFFFu)) & 0x00FFFFFFu;
    };
    const auto pixel_cost = [&]() {
        double per = (cycle_type == 1) ? rdp_per_pixel_2cycle : rdp_per_pixel_1cycle;
        if (zbuffer) {
            per *= rdp_zbuffer_factor;
        }
        return per;
    };
    uint32_t geometry = 0;
    double triangles_total = 0, triangles_culled = 0;
    const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        rsp_cycles += rsp_per_triangle;
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
            return;
        }
        if (!vertices[a].valid || !vertices[b].valid || !vertices[c].valid) {
            return;
        }
        triangles_total += 1;
        // Near plane first, in clip space (x, y as ratios to w are not yet
        // safe): Point.x carries the clip-space w, Point.y a vertex index-free
        // blend, so clip x, y and w together by working on three parallel lists.
        struct Clip { double x, y, w; };
        std::vector<Clip> poly = {
            {vertices[a].cx, vertices[a].cy, vertices[a].cw},
            {vertices[b].cx, vertices[b].cy, vertices[b].cw},
            {vertices[c].cx, vertices[c].cy, vertices[c].cw},
        };
        constexpr double near_w = 1e-2;
        std::vector<Clip> front;
        for (size_t i = 0; i < poly.size(); i++) {
            const Clip& p0 = poly[i];
            const Clip& p1 = poly[(i + 1) % poly.size()];
            const bool in0 = p0.w > near_w, in1 = p1.w > near_w;
            if (in0) {
                front.push_back(p0);
            }
            if (in0 != in1) {
                const double t = (near_w - p0.w) / (p1.w - p0.w);
                front.push_back({p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, near_w});
            }
        }
        if (front.size() < 3) {
            return;  // wholly behind the eye
        }
        std::vector<Point> screen;
        for (const Clip& v : front) {
            screen.push_back({vp_trans_x + (v.x / v.w) * vp_scale_x, vp_trans_y - (v.y / v.w) * vp_scale_y});
        }
        // Screen y runs down, so a face authored counter-clockwise (the usual
        // front) has negative signed area here. G_CULL_BACK (0x400) drops the
        // positive ones, G_CULL_FRONT (0x200) the negative ones.
        const double signed_area = polygon_area(screen);
        if (((geometry & 0x400) && signed_area > 0) || ((geometry & 0x200) && signed_area < 0)) {
            triangles_culled += 1;
            return;  // culled on the RSP: the RDP never sees it
        }
        rdp_cycles += rdp_per_triangle;
        const double w = 320, h = 240;
        screen = clip_edge(screen, [](const Point& p) { return p.x >= 0; },
                           [](const Point& a, const Point& b) { double t = (0 - a.x) / (b.x - a.x); return Point{0, a.y + (b.y - a.y) * t}; });
        screen = clip_edge(screen, [&](const Point& p) { return p.x <= w; },
                           [&](const Point& a, const Point& b) { double t = (w - a.x) / (b.x - a.x); return Point{w, a.y + (b.y - a.y) * t}; });
        screen = clip_edge(screen, [](const Point& p) { return p.y >= 0; },
                           [](const Point& a, const Point& b) { double t = (0 - a.y) / (b.y - a.y); return Point{a.x + (b.x - a.x) * t, 0}; });
        screen = clip_edge(screen, [&](const Point& p) { return p.y <= h; },
                           [&](const Point& a, const Point& b) { double t = (h - a.y) / (b.y - a.y); return Point{a.x + (b.x - a.x) * t, h}; });
        if (screen.size() < 3) {
            return;
        }
        rdp_cycles += std::fabs(polygon_area(screen)) * pixel_cost();
    };

    for (int steps = 0; steps < 200000; steps++) {
        if (!readable(pc, 8)) {
            break;
        }
        const uint32_t w0 = word(rdram, pc);
        const uint32_t w1 = word(rdram, pc + 4);
        pc += 8;
        rdp_cycles += rdp_per_command;
        switch (w0 >> 24) {
            case 0x01: {  // G_VTX
                const uint32_t count = (w0 >> 12) & 0xFF;
                const uint32_t end = (w0 & 0xFF) >> 1;
                const uint32_t first = end >= count ? end - count : 0;
                const uint32_t source = resolve(w1);
                if (combined_dirty) {
                    combined = multiply(modelview, projection);
                    combined_dirty = false;
                }
                for (uint32_t i = 0; i < count && first + i < vertices.size(); i++) {
                    const uint32_t v = source + i * 16;
                    rsp_cycles += rsp_per_vertex;
                    if (!readable(v, 16)) {
                        continue;
                    }
                    const float x = static_cast<float>(half(rdram, v));
                    const float y = static_cast<float>(half(rdram, v + 2));
                    const float z = static_cast<float>(half(rdram, v + 4));
                    const auto& m = combined.m;
                    const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
                    const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
                    const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
                    ScreenVertex& out = vertices[first + i];
                    out.cx = cx;
                    out.cy = cy;
                    out.cw = cw;
                    out.valid = true;
                }
                break;
            }
            case 0x05:  // G_TRI1
                triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                break;
            case 0x06:  // G_TRI2
            case 0x07:  // G_QUAD
                triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case 0xDA: {  // G_MTX; F3DEX2 stores the params XORed with G_MTX_PUSH
                const uint32_t params = (w0 & 0xFF) ^ 1u;
                const uint32_t source = resolve(w1);
                if (!readable(source, 64)) {
                    break;
                }
                const Matrix m = read_matrix(rdram, source);
                rsp_cycles += 60;
                if (params & 4) {  // projection
                    projection = (params & 2) ? m : multiply(m, projection);
                } else {
                    if (params & 1) {
                        modelview_stack.push_back(modelview);
                    }
                    modelview = (params & 2) ? m : multiply(m, modelview);
                }
                combined_dirty = true;
                break;
            }
            case 0xD8: {  // G_POPMTX: w1 bytes of matrices
                for (uint32_t n = w1 / 64; n > 0 && !modelview_stack.empty(); n--) {
                    modelview = modelview_stack.back();
                    modelview_stack.pop_back();
                }
                combined_dirty = true;
                break;
            }
            case 0xDC:  // G_MOVEMEM
                if ((w0 & 0xFF) == 0x08) {  // G_MV_VIEWPORT
                    const uint32_t vp = resolve(w1);
                    if (readable(vp, 16)) {
                        vp_scale_x = half(rdram, vp) / 4.0f;
                        vp_scale_y = half(rdram, vp + 2) / 4.0f;
                        vp_trans_x = half(rdram, vp + 8) / 4.0f;
                        vp_trans_y = half(rdram, vp + 10) / 4.0f;
                    }
                }
                break;
            case 0xD9:  // G_GEOMETRYMODE: keep the bits in w0's mask, then set w1's
                geometry = (geometry & (w0 | 0xFF000000u)) | w1;
                zbuffer = (geometry & 1u) != 0;
                break;
            case 0xE3: {  // G_SETOTHERMODE_H
                const uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - ((w0 & 0xFF) + 1);
                const uint32_t length = (w0 & 0xFF) + 1;
                if (shift <= 20 && shift + length > 20) {
                    cycle_type = (w1 >> 20) & 3;
                }
                break;
            }
            case 0xEF:  // G_RDPSETOTHERMODE: w0 carries othermode-hi
                cycle_type = (w0 >> 20) & 3;
                break;
            case 0xF6: {  // G_FILLRECT, 10.2 fixed point, inclusive in fill mode
                const double lrx = ((w0 >> 12) & 0xFFF) / 4.0, lry = (w0 & 0xFFF) / 4.0;
                const double ulx = ((w1 >> 12) & 0xFFF) / 4.0, uly = (w1 & 0xFFF) / 4.0;
                const double area = std::max(0.0, lrx - ulx + 1) * std::max(0.0, lry - uly + 1);
                rdp_cycles += area * ((cycle_type == 3) ? rdp_per_pixel_fill : pixel_cost());
                break;
            }
            case 0xE4: {  // G_TEXRECT, followed by two RDPHALF words
                const double lrx = ((w0 >> 12) & 0xFFF) / 4.0, lry = (w0 & 0xFFF) / 4.0;
                const double ulx = ((w1 >> 12) & 0xFFF) / 4.0, uly = (w1 & 0xFFF) / 4.0;
                const double area = std::max(0.0, lrx - ulx) * std::max(0.0, lry - uly);
                rdp_cycles += area * ((cycle_type == 2) ? rdp_per_pixel_copy : pixel_cost());
                pc += 16;
                break;
            }
            case 0xDB:  // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {  // G_MW_SEGMENT
                    segment[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFFu;
                }
                break;
            case 0xDE:  // G_DL: 0 pushes a return, 1 is a plain branch
                if (((w0 >> 16) & 0xFF) == 0) {
                    returns.push_back(pc);
                }
                pc = resolve(w1);
                break;
            case 0xDF:  // G_ENDDL
                if (returns.empty()) {
                    steps = 1 << 30;
                } else {
                    pc = returns.back();
                    returns.pop_back();
                }
                break;
            default:
                break;
        }
    }
    const double cycles = std::max(rsp_cycles, rdp_cycles);
    last_culled_fraction = triangles_total > 0 ? triangles_culled / triangles_total : 0;
    return cycles / rcp_hz * 1000.0;
}
