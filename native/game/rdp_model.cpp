// Per-frame drawing-time model: how long a real N64 would take to draw each
// display list the game submits, so the game can be paced the way the console
// paced it.
//
// Knife Edge runs a step from the retrace callback, and only when no graphics
// task is outstanding (func_800D2930 hands the callback the pending count, and
// every callback opens with `bgtz pending, skip`). So on hardware one frame is
// the CPU's step followed by the RCP drawing it, in series, rounded up to the
// next retrace. RT64 draws instantly and the recompiled CPU is fast, so without
// this the game steps on every retrace.
//
// The list is walked as F3DEX2 would: vertices are transformed by the matrices
// the game loaded, triangles are classified the way the microcode classifies
// them, and drawn triangles are clipped to the screen for their pixel area.
//
// Where the costs come from:
//
//   Measured (F3DEX2 RSP cycles, from the F3DEX3 project's performance
//   tables, https://hackern64.github.io/F3DEX3/performance.html):
//     command dispatch 12; small RDP command 14; G_VTX before DMA 16;
//     vertex pair 54 unlit, 73/76/88/91/103/106/118 with 1-7 directional
//     lights; light transform 141 + 39 per extra light; triangle drawn 172,
//     offscreen 27, to clip 32, backface 38, degenerate 42.
//
//   Documented (n64brew, RDP pipeline): 1 cycle per pixel in 1-cycle mode, 2
//     in 2-cycle mode, 64 bits per cycle in fill and copy mode; pipe sync 50,
//     tile sync 33, load sync 25 cycles.
//
//   Assumed, and marked so below: G_MTX cost, clipping work, RDP triangle and
//     span setup, and sustained RDRAM bandwidth (~200 MB/s of the 562.5 MB/s
//     peak). KNIFE_EDGE_RDP_MODEL_CSV records the raw counts so they can be
//     varied offline.
//
// The estimate this returns is the documented and measured cost only. The
// pacing in pipeline_trace.cpp multiplies the RDP side by a factor fitted to
// real-hardware footage (2.7), which absorbs what the assumptions miss.

#include "rdp_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "recomp.h"

namespace {

constexpr double rcp_hz = 62.5e6;  // RSP and RDP clock

// --- RSP, F3DEX2, measured ---
constexpr double rsp_dispatch = 12;
constexpr double rsp_small_rdp_command = 14;
constexpr double rsp_vtx_command = 16;
constexpr double rsp_vertex_pair[8] = {54, 73, 76, 88, 91, 103, 106, 118};  // by light count
constexpr double rsp_light_xfrm_first = 141;
constexpr double rsp_light_xfrm_extra = 39;
constexpr double rsp_tri_draw = 172;
constexpr double rsp_tri_offscreen = 27;
constexpr double rsp_tri_clip = 32;
constexpr double rsp_tri_backface = 38;
constexpr double rsp_tri_degenerate = 42;
// --- RSP, assumed ---
constexpr double rsp_mtx = 60;           // DMA 64 bytes plus a 4x4 multiply
constexpr double rsp_clip_work = 300;    // per clipped triangle, beyond the triangles it emits

// --- RDP, documented ---
constexpr double rdp_sync_pipe = 50;
constexpr double rdp_sync_tile = 33;
constexpr double rdp_sync_load = 25;
constexpr double rdp_fill_copy_pipe = 0.25;  // 64 bits a cycle: four 16-bit pixels
// --- RDP, assumed ---
constexpr double rdp_tri_setup = 30;     // edge-walker setup per triangle

// --- RDP, fitted to real-hardware footage (see pipeline_trace.cpp) ---
// The RDP is bound by RDRAM far more than by its pipeline: every pixel it
// touches is colour (and often Z) read and written, and every texture load is
// read. So each pixel costs its pipeline rate or its memory traffic at an
// effective bandwidth, whichever is slower, and each scanline span a fixed
// overhead. KNIFE_EDGE_MODEL_BW (bytes per RCP cycle) and KNIFE_EDGE_MODEL_SPAN
// (cycles) override the fitted values for experiments.
double env_or(const char* name, double fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::atof(text) : fallback;
}
const double rdp_raw_bytes_per_cycle = env_or("KNIFE_EDGE_MODEL_BW", 1.47);
const double rdp_span = env_or("KNIFE_EDGE_MODEL_SPAN", 36);

// The VI scans the framebuffer out of the same RDRAM 60 times a second, and
// what it takes the RDP does not get: 320x240 at 16 bits is 0.15 bytes per RCP
// cycle, 640x480 (the title screen) 0.59. Reading only one field's lines at
// 640x480 (0.29) fits the footage worse (title 15% fast instead of 2% slow).
double rdp_bytes_per_cycle_at(uint32_t width, uint32_t pixel_size) {
    const double bytes = pixel_size == 3 ? 4.0 : 2.0;
    const double height = width * 3.0 / 4.0;
    const double scanout = width * height * bytes * 60.0 / rcp_hz;
    return std::max(0.1, rdp_raw_bytes_per_cycle - scanout);
}

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

struct Vertex {
    float x = 0, y = 0, w = 1;  // clip space
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

FrameCounts last_counts;
bool trace_this_frame = false;

}  // namespace

// KNIFE_EDGE_MODEL_TRACE: print the rectangles and mode changes of one frame.
extern "C" void knife_edge_model_trace_next(int enable) {
    trace_this_frame = enable != 0;
}

extern "C" const FrameCounts* knife_edge_last_frame_counts(void) {
    return &last_counts;
}

extern "C" double knife_edge_estimate_frame_ms(uint8_t* rdram, uint32_t display_list) {
    FrameCounts n{};
    uint32_t segment[16] = {};
    std::vector<uint32_t> returns;
    std::vector<Matrix> modelview_stack;
    Matrix projection = identity();
    Matrix modelview = identity();
    Matrix combined = identity();
    bool combined_dirty = true;
    bool lights_dirty = true;
    std::array<Vertex, 64> vertices{};
    float vp_scale_x = 160, vp_scale_y = 120, vp_trans_x = 160, vp_trans_y = 120;
    uint32_t geometry = 0;
    uint32_t lights = 1;
    uint32_t cycle_type = 0;   // 0 1-cycle, 1 2-cycle, 2 copy, 3 fill
    uint32_t othermode_l = 0;  // Z_CMP 0x10, Z_UPD 0x20, IM_RD 0x40
    uint32_t image_size = 2;   // G_SETTIMG texel size: 0 4b, 1 8b, 2 16b, 3 32b
    uint32_t color_size = 2;   // G_SETCIMG pixel size
    double rdp_bytes_per_cycle = rdp_bytes_per_cycle_at(320, 2);
    bool framebuffer_seen = false;
    // Scissor, in pixels: G_SETSCISSOR sets it. The title screen draws at
    // 640x480, so the screen size must come from the list, not a constant.
    double scissor_left = 0, scissor_top = 0, scissor_right = 320, scissor_bottom = 240;

    double rsp = 0, rdp = 0;
    uint32_t pc = display_list & 0x00FFFFFFu;
    const auto resolve = [&](uint32_t address) {
        return (segment[(address >> 24) & 0xF] + (address & 0x00FFFFFFu)) & 0x00FFFFFFu;
    };
    // One batch of pixels in the current mode: its pipeline cycles and its
    // RDRAM bytes, costed at whichever is slower.
    const auto count_pixels = [&](double area, bool rectangle) {
        const double pixel_bytes = color_size == 3 ? 4.0 : 2.0;
        double pipe, bytes;
        if (cycle_type >= 2) {  // copy and fill: write only, 64 bits a cycle
            pipe = rdp_fill_copy_pipe;
            bytes = pixel_bytes;
            (cycle_type == 3 ? n.px_fill : n.px_copy) += area;
        } else {
            pipe = cycle_type == 1 ? 2.0 : 1.0;
            bytes = pixel_bytes;                                 // colour write
            if (othermode_l & 0x40) bytes += pixel_bytes;        // IM_RD: colour read
            if (!rectangle && (othermode_l & 0x10)) bytes += 2;  // Z read
            if (!rectangle && (othermode_l & 0x20)) bytes += 2;  // Z write
            if (!rectangle && (othermode_l & 0x30)) (cycle_type == 1 ? n.px_2c_z : n.px_1c_z) += area;
            else (cycle_type == 1 ? n.px_2c : n.px_1c) += area;
        }
        n.pipe_cycles += area * pipe;
        n.pixel_bytes += area * bytes;
        rdp += area * std::max(pipe, bytes / rdp_bytes_per_cycle);
    };

    // Rasterize a screen-space polygon (already on the near side of the eye).
    const auto rasterize = [&](std::vector<Point> screen) {
        const double x0 = scissor_left, y0 = scissor_top, x1 = scissor_right, y1 = scissor_bottom;
        screen = clip_edge(screen, [&](const Point& p) { return p.x >= x0; },
                           [&](const Point& a, const Point& b) { double t = (x0 - a.x) / (b.x - a.x); return Point{x0, a.y + (b.y - a.y) * t}; });
        screen = clip_edge(screen, [&](const Point& p) { return p.x <= x1; },
                           [&](const Point& a, const Point& b) { double t = (x1 - a.x) / (b.x - a.x); return Point{x1, a.y + (b.y - a.y) * t}; });
        screen = clip_edge(screen, [&](const Point& p) { return p.y >= y0; },
                           [&](const Point& a, const Point& b) { double t = (y0 - a.y) / (b.y - a.y); return Point{a.x + (b.x - a.x) * t, y0}; });
        screen = clip_edge(screen, [&](const Point& p) { return p.y <= y1; },
                           [&](const Point& a, const Point& b) { double t = (y1 - a.y) / (b.y - a.y); return Point{a.x + (b.x - a.x) * t, y1}; });
        if (screen.size() < 3) {
            return;  // scissored away
        }
        double top = y1, bottom = y0;
        for (const Point& p : screen) {
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        const double spans = std::max(0.0, std::ceil(bottom) - std::floor(top));
        n.spans += spans;
        rdp += spans * rdp_span;
        count_pixels(std::fabs(polygon_area(screen)), false);
    };

    const auto project = [&](double x, double y, double w) {
        return Point{vp_trans_x + (x / w) * vp_scale_x, vp_trans_y - (y / w) * vp_scale_y};
    };

    const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size() ||
            !vertices[a].valid || !vertices[b].valid || !vertices[c].valid) {
            rsp += rsp_tri_offscreen;
            return;
        }
        const Vertex* v[3] = {&vertices[a], &vertices[b], &vertices[c]};
        // Clip codes as F3DEX2 computes them: outside the screen, and outside
        // the guard band (clip ratio 2) or behind the near plane.
        uint32_t screen_all = 0xF, guard_any = 0;
        constexpr double near_w = 1e-2;
        for (const Vertex* p : v) {
            uint32_t code = 0;
            if (p->x < -p->w) code |= 1;
            if (p->x > p->w) code |= 2;
            if (p->y < -p->w) code |= 4;
            if (p->y > p->w) code |= 8;
            screen_all &= code;
            if (p->w <= near_w || std::fabs(p->x) > 2 * p->w || std::fabs(p->y) > 2 * p->w) {
                guard_any = 1;
            }
        }
        if (screen_all != 0) {
            n.tris_offscreen++;
            rsp += rsp_tri_offscreen;
            return;
        }
        if (guard_any) {
            // Clipped on the RSP against the near plane (in clip space), then
            // emitted as a fan; the guard band itself is left to the scissor.
            n.tris_clipped++;
            rsp += rsp_tri_clip + rsp_clip_work;
            struct Clip { double x, y, w; };
            std::vector<Clip> front;
            for (int i = 0; i < 3; i++) {
                const Vertex& p0 = *v[i];
                const Vertex& p1 = *v[(i + 1) % 3];
                const bool in0 = p0.w > near_w, in1 = p1.w > near_w;
                if (in0) {
                    front.push_back({p0.x, p0.y, p0.w});
                }
                if (in0 != in1) {
                    const double t = (near_w - p0.w) / (p1.w - p0.w);
                    front.push_back({p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, near_w});
                }
            }
            if (front.size() < 3) {
                return;
            }
            std::vector<Point> screen;
            for (const Clip& p : front) {
                screen.push_back(project(p.x, p.y, p.w));
            }
            const double signed_area = polygon_area(screen);
            if (((geometry & 0x400) && signed_area > 0) || ((geometry & 0x200) && signed_area < 0)) {
                n.tris_backface++;
                return;
            }
            const double emitted = static_cast<double>(screen.size() - 2);
            n.tris_drawn += emitted;
            rsp += emitted * rsp_tri_draw;
            rdp += emitted * rdp_tri_setup;
            rasterize(screen);
            return;
        }
        std::vector<Point> screen = {project(v[0]->x, v[0]->y, v[0]->w), project(v[1]->x, v[1]->y, v[1]->w),
                                     project(v[2]->x, v[2]->y, v[2]->w)};
        // Screen y runs down, so a counter-clockwise (front) face has negative
        // signed area here. G_CULL_BACK is 0x400, G_CULL_FRONT 0x200.
        const double signed_area = polygon_area(screen);
        if (((geometry & 0x400) && signed_area > 0) || ((geometry & 0x200) && signed_area < 0)) {
            n.tris_backface++;
            rsp += rsp_tri_backface;
            return;
        }
        if (std::fabs(signed_area) < 0.25) {
            n.tris_degenerate++;
            rsp += rsp_tri_degenerate;
            return;
        }
        n.tris_drawn++;
        rsp += rsp_tri_draw;
        rdp += rdp_tri_setup;
        rasterize(screen);
    };

    for (int steps = 0; steps < 200000; steps++) {
        if (!readable(pc, 8)) {
            break;
        }
        const uint32_t w0 = word(rdram, pc);
        const uint32_t w1 = word(rdram, pc + 4);
        pc += 8;
        n.commands++;
        rsp += rsp_dispatch;
        const uint32_t op = w0 >> 24;
        if (op >= 0xE0) {
            n.rdp_commands++;
            rsp += rsp_small_rdp_command;  // passed through to the RDP
        }
        switch (op) {
            case 0x01: {  // G_VTX
                const uint32_t count = (w0 >> 12) & 0xFF;
                const uint32_t end = (w0 & 0xFF) >> 1;
                const uint32_t first = end >= count ? end - count : 0;
                const uint32_t source = resolve(w1);
                const bool lit = (geometry & 0x00200000u) != 0;  // G_LIGHTING
                const uint32_t light_count = std::min<uint32_t>(lights, 7);
                rsp += rsp_vtx_command;
                if (lit && lights_dirty) {
                    rsp += rsp_light_xfrm_first + rsp_light_xfrm_extra * (std::max<uint32_t>(light_count, 1) - 1);
                    n.light_xfrms++;
                    lights_dirty = false;
                }
                rsp += rsp_vertex_pair[lit ? light_count : 0] * std::ceil(count / 2.0);
                n.vertices += count;
                if (lit) {
                    n.vertices_lit += count;
                }
                if (combined_dirty) {
                    combined = multiply(modelview, projection);
                    combined_dirty = false;
                }
                for (uint32_t i = 0; i < count && first + i < vertices.size(); i++) {
                    const uint32_t address = source + i * 16;
                    if (!readable(address, 16)) {
                        continue;
                    }
                    const float x = static_cast<float>(half(rdram, address));
                    const float y = static_cast<float>(half(rdram, address + 2));
                    const float z = static_cast<float>(half(rdram, address + 4));
                    const auto& m = combined.m;
                    Vertex& out = vertices[first + i];
                    out.x = x * m[0] + y * m[4] + z * m[8] + m[12];
                    out.y = x * m[1] + y * m[5] + z * m[9] + m[13];
                    out.w = x * m[3] + y * m[7] + z * m[11] + m[15];
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
                n.matrices++;
                rsp += rsp_mtx;
                if (!readable(source, 64)) {
                    break;
                }
                const Matrix m = read_matrix(rdram, source);
                if (params & 4) {  // projection
                    projection = (params & 2) ? m : multiply(m, projection);
                } else {
                    if (params & 1) {
                        modelview_stack.push_back(modelview);
                    }
                    modelview = (params & 2) ? m : multiply(m, modelview);
                    lights_dirty = true;
                }
                combined_dirty = true;
                break;
            }
            case 0xD8: {  // G_POPMTX: w1 bytes of matrices
                for (uint32_t k = w1 / 64; k > 0 && !modelview_stack.empty(); k--) {
                    modelview = modelview_stack.back();
                    modelview_stack.pop_back();
                }
                combined_dirty = true;
                lights_dirty = true;
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
                } else {
                    lights_dirty = true;  // a light, most likely
                }
                break;
            case 0xD9:  // G_GEOMETRYMODE: keep the bits in w0's mask, then set w1's
                geometry = (geometry & (w0 | 0xFF000000u)) | w1;
                break;
            case 0xE2: {  // G_SETOTHERMODE_L
                const uint32_t length = (w0 & 0xFF) + 1;
                const uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - length;
                const uint32_t mask = (length >= 32 ? 0xFFFFFFFFu : ((1u << length) - 1)) << shift;
                othermode_l = (othermode_l & ~mask) | (w1 & mask);
                break;
            }
            case 0xE3: {  // G_SETOTHERMODE_H
                const uint32_t length = (w0 & 0xFF) + 1;
                const uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - length;
                if (shift <= 20 && shift + length > 20) {
                    cycle_type = (w1 >> 20) & 3;
                }
                break;
            }
            case 0xEF:  // G_RDPSETOTHERMODE: w0 othermode-hi, w1 othermode-lo
                cycle_type = (w0 >> 20) & 3;
                othermode_l = w1;
                break;
            case 0xE7:  // G_RDPPIPESYNC
                n.syncs++;
                n.sync_cycles += rdp_sync_pipe;
                rdp += rdp_sync_pipe;
                break;
            case 0xE8:  // G_RDPTILESYNC
                n.syncs++;
                n.sync_cycles += rdp_sync_tile;
                rdp += rdp_sync_tile;
                break;
            case 0xE6:  // G_RDPLOADSYNC
                n.syncs++;
                n.sync_cycles += rdp_sync_load;
                rdp += rdp_sync_load;
                break;
            case 0xED:  // G_SETSCISSOR: 10.2 fixed-point corners
                scissor_left = ((w0 >> 12) & 0xFFF) / 4.0;
                scissor_top = (w0 & 0xFFF) / 4.0;
                scissor_right = ((w1 >> 12) & 0xFFF) / 4.0;
                scissor_bottom = (w1 & 0xFFF) / 4.0;
                break;
            case 0xFD:  // G_SETTIMG
                image_size = (w0 >> 19) & 3;
                break;
            case 0xFF:  // G_SETCIMG
                color_size = (w0 >> 19) & 3;
                if (!framebuffer_seen) {  // the first image set is the frame's own
                    rdp_bytes_per_cycle = rdp_bytes_per_cycle_at((w0 & 0xFFF) + 1, color_size);
                    framebuffer_seen = true;
                }
                if (trace_this_frame) {
                    std::fprintf(stderr, "[model-trace] setcimg size %u width %u addr %08X\n", color_size,
                                 (w0 & 0xFFF) + 1, w1);
                }
                break;
            case 0xF3: {  // G_LOADBLOCK: lrs is the texel count minus one
                const double texels = ((w1 >> 12) & 0xFFF) + 1;
                const double bytes = texels * (image_size == 0 ? 0.5 : double(1u << (image_size - 1)));
                n.texture_bytes += bytes;
                rdp += bytes / rdp_bytes_per_cycle;
                break;
            }
            case 0xF4: {  // G_LOADTILE: 10.2 fixed-point corners
                const double width = (((w1 >> 12) & 0xFFF) - ((w0 >> 12) & 0xFFF)) / 4.0 + 1;
                const double height = ((w1 & 0xFFF) - (w0 & 0xFFF)) / 4.0 + 1;
                const double bytes = std::max(0.0, width) * std::max(0.0, height) *
                                     (image_size == 0 ? 0.5 : double(1u << (image_size - 1)));
                n.texture_bytes += bytes;
                rdp += bytes / rdp_bytes_per_cycle;
                break;
            }
            case 0xF0: {  // G_LOADTLUT: count in the lrs field, 16-bit entries
                const double bytes = (((w1 >> 14) & 0x3FF) + 1) * 2.0;
                n.texture_bytes += bytes;
                rdp += bytes / rdp_bytes_per_cycle;
                break;
            }
            case 0xF6: {  // G_FILLRECT, 10.2 fixed point, inclusive in fill and copy mode
                const double lrx = ((w0 >> 12) & 0xFFF) / 4.0, lry = (w0 & 0xFFF) / 4.0;
                const double ulx = ((w1 >> 12) & 0xFFF) / 4.0, uly = (w1 & 0xFFF) / 4.0;
                if (trace_this_frame) {
                    std::fprintf(stderr, "[model-trace] fillrect %.0f,%.0f-%.0f,%.0f cycle %u othermode_l %08X color_size %u\n",
                                 ulx, uly, lrx, lry, cycle_type, othermode_l, color_size);
                }
                const double inclusive = cycle_type >= 2 ? 1 : 0;
                const double rows = std::max(0.0, lry - uly + inclusive);
                n.spans += rows;
                rdp += rows * rdp_span;
                count_pixels(std::max(0.0, lrx - ulx + inclusive) * rows, true);
                break;
            }
            case 0xE4: {  // G_TEXRECT, followed by two RDPHALF words
                const double lrx = ((w0 >> 12) & 0xFFF) / 4.0, lry = (w0 & 0xFFF) / 4.0;
                const double ulx = ((w1 >> 12) & 0xFFF) / 4.0, uly = (w1 & 0xFFF) / 4.0;
                if (trace_this_frame) {
                    std::fprintf(stderr, "[model-trace] texrect %.0f,%.0f-%.0f,%.0f cycle %u othermode_l %08X\n",
                                 ulx, uly, lrx, lry, cycle_type, othermode_l);
                }
                const double inclusive = cycle_type >= 2 ? 1 : 0;
                const double rows = std::max(0.0, lry - uly + inclusive);
                n.spans += rows;
                rdp += rows * rdp_span;
                count_pixels(std::max(0.0, lrx - ulx + inclusive) * rows, true);
                pc += 16;
                break;
            }
            case 0xDB:  // G_MOVEWORD
                if (((w0 >> 16) & 0xFF) == 0x06) {  // G_MW_SEGMENT
                    segment[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFFu;
                } else if (((w0 >> 16) & 0xFF) == 0x02) {  // G_MW_NUMLIGHT: 24 bytes a light
                    lights = w1 / 24;
                    lights_dirty = true;
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
    n.rsp_ms = rsp / rcp_hz * 1000.0;
    n.rdp_ms = rdp / rcp_hz * 1000.0;
    last_counts = n;
    trace_this_frame = false;
    // The microcode feeds the RDP through a FIFO, so the two overlap and the
    // task takes about as long as the slower of them.
    return std::max(n.rsp_ms, n.rdp_ms);
}
