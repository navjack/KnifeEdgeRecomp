// map-recovery statically recovers CPU code maps for N64Recomp symbol files.
//
//	sweep   report code entry points the map is missing (jal targets, address
//	        constants, pointer words in data blocks) and jal targets outside
//	        every mapped section
//	locate  vote on ROM-to-VRAM offsets for unmapped jal targets to find
//	        overlays in unmapped ROM
//	match   name functions by comparing operand-masked bytes against a
//	        reviewed reference map (libultra identification)
package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const jrRA = 0x03E00008

type function struct {
	Name string
	VRAM uint32
	Size uint32
}

type section struct {
	Name  string
	ROM   uint32
	VRAM  uint32
	Size  uint32
	Funcs []function
}

func (s section) contains(vram uint32) bool { return vram >= s.VRAM && vram < s.VRAM+s.Size }
func (s section) romOf(vram uint32) uint32  { return s.ROM + (vram - s.VRAM) }

type codeMap struct {
	sections []section
	starts   map[uint32]bool
	sorted   []function
}

var (
	functionPattern = regexp.MustCompile(`\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*size\s*=\s*(0x[0-9A-Fa-f]+)`)
	fieldPattern    = regexp.MustCompile(`^\s*(name|rom|vram|size)\s*=\s*(?:"([^"]*)"|(0x[0-9A-Fa-f]+|\d+))`)
	generatedName   = regexp.MustCompile(`^func_[0-9A-Fa-f]{8}$`)
)

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "map-recovery: "+format+"\n", args...)
	os.Exit(1)
}

func parseNum(text string) uint32 {
	value, err := strconv.ParseUint(text, 0, 32)
	if err != nil {
		fatalf("invalid number %q", text)
	}
	return uint32(value)
}

func readROM(path string) []byte {
	rom, err := os.ReadFile(path)
	if err != nil {
		fatalf("read %s: %v", path, err)
	}
	if len(rom) < 0x1000 || binary.BigEndian.Uint32(rom) != 0x80371240 {
		fatalf("%s is not a big-endian .z64 ROM", path)
	}
	return rom
}

func word(rom []byte, offset uint32) (uint32, bool) {
	if uint64(offset)+4 > uint64(len(rom)) {
		return 0, false
	}
	return binary.BigEndian.Uint32(rom[offset:]), true
}

func loadMap(path string) *codeMap {
	file, err := os.Open(path)
	if err != nil {
		fatalf("open %s: %v", path, err)
	}
	defer file.Close()

	m := &codeMap{starts: map[uint32]bool{}}
	var current *section
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	for scanner.Scan() {
		line := scanner.Text()
		if strings.TrimSpace(line) == "[[section]]" {
			m.sections = append(m.sections, section{})
			current = &m.sections[len(m.sections)-1]
			continue
		}
		if current == nil {
			continue
		}
		if matches := functionPattern.FindAllStringSubmatch(line, -1); matches != nil {
			for _, match := range matches {
				current.Funcs = append(current.Funcs, function{match[1], parseNum(match[2]), parseNum(match[3])})
			}
			continue
		}
		if match := fieldPattern.FindStringSubmatch(line); match != nil {
			switch match[1] {
			case "name":
				current.Name = match[2]
			case "rom":
				current.ROM = parseNum(match[3])
			case "vram":
				current.VRAM = parseNum(match[3])
			case "size":
				current.Size = parseNum(match[3])
			}
		}
	}
	if err := scanner.Err(); err != nil {
		fatalf("read %s: %v", path, err)
	}
	for _, s := range m.sections {
		for _, f := range s.Funcs {
			m.starts[f.VRAM] = true
			m.sorted = append(m.sorted, f)
		}
	}
	sort.Slice(m.sorted, func(i, j int) bool { return m.sorted[i].VRAM < m.sorted[j].VRAM })
	return m
}

func (m *codeMap) inAnySection(vram uint32) bool {
	for _, s := range m.sections {
		if s.contains(vram) {
			return true
		}
	}
	return false
}

// inside reports whether vram lies within a mapped function body. Overlays
// may share a VRAM range, so a few preceding functions are checked.
func (m *codeMap) inside(vram uint32) bool {
	i := sort.Search(len(m.sorted), func(i int) bool { return m.sorted[i].VRAM > vram }) - 1
	for j := i; j >= 0 && j > i-64; j-- {
		if vram < m.sorted[j].VRAM+m.sorted[j].Size {
			return true
		}
	}
	return false
}

func isPrologue(w uint32) bool { return w&0xFFFF8000 == 0x27BD8000 }

func jalTarget(w, pc uint32) (uint32, bool) {
	if w>>26 != 3 {
		return 0, false
	}
	return (pc+4)&0xF0000000 | (w&0x03FFFFFF)<<2, true
}

func branchTarget(w, pc uint32) (uint32, bool) {
	relative := pc + 4 + uint32(int32(int16(w&0xFFFF))<<2)
	switch w >> 26 {
	case 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17:
		return relative, true
	case 0x01:
		switch (w >> 16) & 0x1F {
		case 0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13:
			return relative, true
		}
	case 0x11:
		if (w>>21)&0x1F == 8 {
			return relative, true
		}
	}
	return 0, false
}

// forEachInstruction visits every instruction of every mapped function.
func (m *codeMap) forEachInstruction(rom []byte, visit func(f function, pc, w uint32, first bool)) {
	for _, s := range m.sections {
		for _, f := range s.Funcs {
			for pc := f.VRAM; pc < f.VRAM+f.Size; pc += 4 {
				w, ok := word(rom, s.romOf(pc))
				if !ok {
					break
				}
				visit(f, pc, w, pc == f.VRAM)
			}
		}
	}
}

func externalJalTargets(rom []byte, m *codeMap) map[uint32]int {
	targets := map[uint32]int{}
	m.forEachInstruction(rom, func(f function, pc, w uint32, first bool) {
		// Targets outside KSEG0 RDRAM are misdecoded data, not calls.
		if target, ok := jalTarget(w, pc); ok && target < 0x80800000 && !m.inAnySection(target) {
			targets[target]++
		}
	})
	return targets
}

type multiFlag []string

func (f *multiFlag) String() string     { return strings.Join(*f, ",") }
func (f *multiFlag) Set(v string) error { *f = append(*f, v); return nil }

func runSweep(args []string) {
	flags := flag.NewFlagSet("sweep", flag.ExitOnError)
	romPath := flags.String("rom", "", "big-endian .z64 ROM")
	mapPath := flags.String("map", "", "N64Recomp symbol TOML")
	listExternal := flags.Bool("externals", false, "list every external jal target")
	var dataBlocks multiFlag
	flags.Var(&dataBlocks, "data", "ROM block to scan for code pointers, as rom:size (repeatable)")
	flags.Parse(args)
	rom := readROM(*romPath)
	m := loadMap(*mapPath)

	type finding struct {
		addr uint32
		kind string
		from string
	}
	var findings []finding
	seen := map[uint32]bool{}
	report := func(addr uint32, kind, from string) {
		if !seen[addr] {
			seen[addr] = true
			findings = append(findings, finding{addr, kind, from})
		}
	}
	// A constant or data word is only an entry point when it lands on a
	// prologue, or outside every function right after a return. Addresses in
	// the middle of a function are jump-table cases.
	checkPointer := func(addr uint32, kind, from string) {
		if addr&3 != 0 || m.starts[addr] {
			return
		}
		for _, s := range m.sections {
			if !s.contains(addr) {
				continue
			}
			w, ok := word(rom, s.romOf(addr))
			if !ok {
				continue
			}
			previous, _ := word(rom, s.romOf(addr)-8)
			afterReturn := addr >= s.VRAM+8 && previous == jrRA
			if isPrologue(w) || (!m.inside(addr) && afterReturn) {
				report(addr, kind, from)
				return
			}
		}
	}

	var high [32]uint32
	var highValid [32]bool
	m.forEachInstruction(rom, func(f function, pc, w uint32, first bool) {
		if first {
			highValid = [32]bool{}
		}
		from := fmt.Sprintf("%s+0x%X", f.Name, pc-f.VRAM)
		if target, ok := jalTarget(w, pc); ok {
			if m.inAnySection(target) && !m.starts[target] {
				report(target, "jal", from)
			}
			return
		}
		op, rs, rt := w>>26, (w>>21)&0x1F, (w>>16)&0x1F
		switch op {
		case 0x0F:
			high[rt], highValid[rt] = w&0xFFFF, true
		case 0x09, 0x0D:
			if highValid[rs] {
				low := uint32(int32(int16(w & 0xFFFF)))
				if op == 0x0D {
					low = w & 0xFFFF
				}
				checkPointer(high[rs]<<16+low, "constant", from)
			}
		}
	})

	for _, block := range dataBlocks {
		parts := strings.Split(block, ":")
		if len(parts) != 2 {
			fatalf("-data expects rom:size, got %q", block)
		}
		start, size := parseNum(parts[0]), parseNum(parts[1])
		for offset := start &^ 3; offset < start+size; offset += 4 {
			w, ok := word(rom, offset)
			if !ok {
				break
			}
			if w >= 0x80000000 && w < 0x80800000 {
				checkPointer(w, "data", fmt.Sprintf("rom 0x%X", offset))
			}
		}
	}

	sort.Slice(findings, func(i, j int) bool { return findings[i].addr < findings[j].addr })
	for _, f := range findings {
		fmt.Printf("unmapped %-8s 0x%08X from %s\n", f.kind, f.addr, f.from)
	}
	externals := externalJalTargets(rom, m)
	if len(externals) > 0 {
		addresses := make([]uint32, 0, len(externals))
		for addr := range externals {
			addresses = append(addresses, addr)
		}
		sort.Slice(addresses, func(i, j int) bool { return addresses[i] < addresses[j] })
		fmt.Printf("external jal targets: %d (0x%08X-0x%08X)\n", len(addresses), addresses[0], addresses[len(addresses)-1])
		if *listExternal {
			for _, addr := range addresses {
				fmt.Printf("external 0x%08X calls=%d\n", addr, externals[addr])
			}
		}
	}
	fmt.Printf("sections=%d functions=%d unmapped=%d external=%d\n", len(m.sections), len(m.sorted), len(findings), len(externals))
	if len(findings) > 0 || len(externals) > 0 {
		os.Exit(1)
	}
}

func runLocate(args []string) {
	flags := flag.NewFlagSet("locate", flag.ExitOnError)
	romPath := flags.String("rom", "", "big-endian .z64 ROM")
	mapPath := flags.String("map", "", "N64Recomp symbol TOML")
	startText := flags.String("start", "0x1000", "first ROM offset to search")
	endText := flags.String("end", "0", "ROM offset to stop searching (0 = ROM end)")
	top := flags.Int("top", 8, "offsets to print")
	loose := flags.Bool("loose", false, "count every prologue, not only those after a return or padding")
	flags.Parse(args)
	rom := readROM(*romPath)
	m := loadMap(*mapPath)
	start, end := parseNum(*startText)&^3, parseNum(*endText)
	if end == 0 || end > uint32(len(rom)) {
		end = uint32(len(rom))
	}

	covered := func(offset uint32) bool {
		for _, s := range m.sections {
			if offset >= s.ROM && offset < s.ROM+s.Size {
				return true
			}
		}
		return false
	}
	// Function starts in unmapped ROM: any word after a return pair (leaf
	// functions have no prologue), or a prologue after padding.
	var entries []uint32
	entrySet := map[uint32]bool{}
	for offset := start; offset+4 <= end; offset += 4 {
		w, _ := word(rom, offset)
		if w == 0 || covered(offset) || offset < 8 {
			continue
		}
		returnPair, _ := word(rom, offset-8)
		padding, _ := word(rom, offset-4)
		if returnPair == jrRA || (isPrologue(w) && (*loose || padding == 0)) {
			entries = append(entries, offset)
			entrySet[offset] = true
		}
	}

	targets := externalJalTargets(rom, m)
	votes := map[uint32]int{}
	for target := range targets {
		for _, entry := range entries {
			votes[target-entry]++
		}
	}
	type candidate struct {
		delta uint32
		votes int
	}
	var ranked []candidate
	for delta, count := range votes {
		if count > 1 {
			ranked = append(ranked, candidate{delta, count})
		}
	}
	sort.Slice(ranked, func(i, j int) bool {
		if ranked[i].votes != ranked[j].votes {
			return ranked[i].votes > ranked[j].votes
		}
		return ranked[i].delta < ranked[j].delta
	})
	fmt.Printf("external targets=%d candidate entries=%d\n", len(targets), len(entries))
	for i, c := range ranked {
		if i == *top {
			break
		}
		low, high := uint32(0xFFFFFFFF), uint32(0)
		for target := range targets {
			if entrySet[target-c.delta] {
				low, high = min(low, target), max(high, target)
			}
		}
		fmt.Printf("rom = vram - 0x%08X  votes=%d/%d  matched vram 0x%08X-0x%08X rom 0x%X-0x%X\n",
			c.delta, c.votes, len(targets), low, high, low-c.delta, high-c.delta)
	}
}

// maskOperands clears jump targets and 16-bit immediates that relocate
// between builds, leaving the instruction structure to compare.
func maskOperands(w uint32) uint32 {
	switch op := w >> 26; {
	case op == 0x02 || op == 0x03:
		return w &^ 0x03FFFFFF
	case op == 0x08 || op == 0x09 || op == 0x0D || op == 0x0F,
		op >= 0x20 && op <= 0x2E,
		op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3D:
		return w &^ 0xFFFF
	}
	return w
}

func functionHashes(rom []byte, m *codeMap, want func(function) bool) map[[32]byte][]function {
	hashes := map[[32]byte][]function{}
	buffer := make([]byte, 4)
	for _, s := range m.sections {
		for _, f := range s.Funcs {
			if f.Size < 0x20 || !want(f) {
				continue
			}
			hash := sha256.New()
			binary.Write(hash, binary.BigEndian, f.Size)
			for pc := f.VRAM; pc < f.VRAM+f.Size; pc += 4 {
				w, _ := word(rom, s.romOf(pc))
				binary.BigEndian.PutUint32(buffer, maskOperands(w))
				hash.Write(buffer)
			}
			var key [32]byte
			copy(key[:], hash.Sum(nil))
			hashes[key] = append(hashes[key], f)
		}
	}
	return hashes
}

func runMatch(args []string) {
	flags := flag.NewFlagSet("match", flag.ExitOnError)
	refROMPath := flags.String("ref-rom", "", "reference ROM")
	refMapPath := flags.String("ref-map", "", "reviewed reference symbol TOML")
	romPath := flags.String("rom", "", "target ROM")
	mapPath := flags.String("map", "", "target symbol TOML")
	write := flags.Bool("write", false, "rename unambiguous matches in the target map")
	// Generated C uses these names directly, so libc-style names from a
	// reference would collide with host symbols.
	namesText := flags.String("names", `^(__)?os[A-Z]`, "regular expression for reference names to adopt")
	flags.Parse(args)
	names, err := regexp.Compile(*namesText)
	if err != nil {
		fatalf("invalid -names expression: %v", err)
	}

	reference := functionHashes(readROM(*refROMPath), loadMap(*refMapPath), func(f function) bool {
		return names.MatchString(f.Name)
	})
	// A library function mapped in several sections is still one name.
	for key, named := range reference {
		distinct := named[:1]
		for _, f := range named[1:] {
			if f.Name != distinct[0].Name {
				distinct = named
				break
			}
		}
		reference[key] = distinct
	}
	target := functionHashes(readROM(*romPath), loadMap(*mapPath), func(f function) bool {
		return generatedName.MatchString(f.Name)
	})

	renames := map[string]string{}
	var lines []string
	for key, candidates := range target {
		named, ok := reference[key]
		if !ok {
			continue
		}
		unique := len(named) == 1 && len(candidates) == 1
		for _, f := range candidates {
			status := "ambiguous"
			if unique {
				status = "match"
				renames[f.Name] = named[0].Name
			}
			lines = append(lines, fmt.Sprintf("%-9s 0x%08X %s -> %s", status, f.VRAM, f.Name, named[0].Name))
		}
	}
	sort.Strings(lines)
	for _, line := range lines {
		fmt.Println(line)
	}
	fmt.Printf("renames=%d\n", len(renames))

	if *write && len(renames) > 0 {
		data, err := os.ReadFile(*mapPath)
		if err != nil {
			fatalf("read %s: %v", *mapPath, err)
		}
		text := string(data)
		for from, to := range renames {
			text = strings.ReplaceAll(text, fmt.Sprintf("name = %q", from), fmt.Sprintf("name = %q", to))
		}
		if err := os.WriteFile(*mapPath, []byte(text), 0o644); err != nil {
			fatalf("write %s: %v", *mapPath, err)
		}
	}
}

func main() {
	if len(os.Args) < 2 {
		fatalf("usage: map-recovery sweep|locate|match [flags]")
	}
	switch os.Args[1] {
	case "sweep":
		runSweep(os.Args[2:])
	case "locate":
		runLocate(os.Args[2:])
	case "match":
		runMatch(os.Args[2:])
	default:
		fatalf("unknown command %q", os.Args[1])
	}
}
