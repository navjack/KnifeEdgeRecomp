package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type requiredSymbol struct {
	Address     string `json:"address"`
	Name        string `json:"name"`
	Replacement string `json:"replacement"`
}

type hardwareLock struct {
	Device              string   `json:"device"`
	Acquire             string   `json:"acquire"`
	Release             string   `json:"release"`
	NativeCallFragments []string `json:"native_call_fragments"`
}

type manifest struct {
	SymbolMap       string           `json:"symbol_map"`
	GeneratedDir    string           `json:"generated_dir"`
	RequiredSymbols []requiredSymbol `json:"required_symbols"`
	HardwareLocks   []hardwareLock   `json:"hardware_locks"`
}

type generatedFunction struct {
	Name string
	File string
	Body string
}

var symbolPattern = regexp.MustCompile(`\{\s*name\s*=\s*"([^"]+)"\s*,\s*vram\s*=\s*(0x[0-9A-Fa-f]+)`)
var functionPattern = regexp.MustCompile(`RECOMP_FUNC void ([A-Za-z0-9_]+)\s*\(`)
var callPattern = regexp.MustCompile(`\b([A-Za-z_][A-Za-z0-9_]*)\(rdram, ctx\);`)

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "runtime-boundary audit: "+format+"\n", args...)
	os.Exit(1)
}

func readFile(path string) []byte {
	data, err := os.ReadFile(path)
	if err != nil {
		fatalf("read %s: %v", path, err)
	}
	return data
}

func parseAddress(text string) uint64 {
	value, err := strconv.ParseUint(strings.TrimPrefix(text, "0x"), 16, 64)
	if err != nil {
		fatalf("invalid address %q", text)
	}
	return value
}

func parseSymbols(path string) map[uint64][]string {
	result := make(map[uint64][]string)
	for _, match := range symbolPattern.FindAllSubmatch(readFile(path), -1) {
		address := parseAddress(string(match[2]))
		name := string(match[1])
		result[address] = append(result[address], name)
	}
	return result
}

func parseGenerated(dir string) []generatedFunction {
	paths, err := filepath.Glob(filepath.Join(dir, "*.c"))
	if err != nil {
		fatalf("enumerate generated files: %v", err)
	}
	sort.Strings(paths)
	var result []generatedFunction
	for _, path := range paths {
		text := string(readFile(path))
		matches := functionPattern.FindAllStringSubmatchIndex(text, -1)
		for i, match := range matches {
			end := len(text)
			if i+1 < len(matches) {
				end = matches[i+1][0]
			}
			result = append(result, generatedFunction{
				Name: text[match[2]:match[3]],
				File: path,
				Body: text[match[0]:end],
			})
		}
	}
	return result
}

func main() {
	manifestPath := flag.String("manifest", "", "runtime-boundary manifest to audit")
	flag.Parse()
	if *manifestPath == "" || flag.NArg() != 0 {
		fatalf("usage: go run ./tools/recomp-boundary-audit -manifest GAME/config/runtime-boundaries.json")
	}

	var config manifest
	if err := json.Unmarshal(readFile(*manifestPath), &config); err != nil {
		fatalf("parse %s: %v", *manifestPath, err)
	}
	base := filepath.Dir(*manifestPath)
	symbolPath := filepath.Clean(filepath.Join(base, config.SymbolMap))
	generatedDir := filepath.Clean(filepath.Join(base, config.GeneratedDir))
	symbols := parseSymbols(symbolPath)
	functions := parseGenerated(generatedDir)
	functionsByName := make(map[string]generatedFunction, len(functions))
	for _, function := range functions {
		functionsByName[function.Name] = function
	}

	type reachKey struct {
		Function string
		Fragment string
	}
	reachMemo := make(map[reachKey]bool)
	var reachesFragment func(string, string, map[string]bool) bool
	reachesFragment = func(functionName string, fragment string, visiting map[string]bool) bool {
		key := reachKey{Function: functionName, Fragment: fragment}
		if value, found := reachMemo[key]; found {
			return value
		}
		if visiting[functionName] {
			return false
		}
		function, found := functionsByName[functionName]
		if !found {
			return false
		}
		if strings.Contains(function.Body, fragment) {
			reachMemo[key] = true
			return true
		}
		visiting[functionName] = true
		defer delete(visiting, functionName)
		for _, match := range callPattern.FindAllStringSubmatch(function.Body, -1) {
			if reachesFragment(match[1], fragment, visiting) {
				reachMemo[key] = true
				return true
			}
		}
		reachMemo[key] = false
		return false
	}

	var problems []string
	for _, required := range config.RequiredSymbols {
		address := parseAddress(required.Address)
		actual, found := symbols[address]
		if !found {
			problems = append(problems, fmt.Sprintf("0x%08X is absent from %s; expected %s", address, symbolPath, required.Name))
		} else {
			matched := false
			for _, name := range actual {
				matched = matched || name == required.Name
			}
			if !matched {
				problems = append(problems, fmt.Sprintf("0x%08X is named %s; expected %s", address, strings.Join(actual, ", "), required.Name))
			}
		}

		replacement := required.Replacement
		if replacement == "" {
			replacement = required.Name + "_recomp"
		}
		needle := replacement + "(rdram, ctx)"
		seen := false
		for _, function := range functions {
			if strings.Contains(function.Body, needle) {
				seen = true
				break
			}
		}
		if !seen {
			problems = append(problems, fmt.Sprintf("%s at 0x%08X never dispatches to %s in generated code", required.Name, address, replacement))
		}
	}

	for _, function := range functions {
		for _, lock := range config.HardwareLocks {
			native := ""
			for _, fragment := range lock.NativeCallFragments {
				if reachesFragment(function.Name, fragment, make(map[string]bool)) {
					native = fragment
					break
				}
			}
			if native == "" {
				continue
			}
			acquire := strings.Contains(function.Body, lock.Acquire+"(rdram, ctx)")
			release := strings.Contains(function.Body, lock.Release+"(rdram, ctx)")
			if acquire || release {
				kind := "release"
				if acquire && release {
					kind = "acquire/release"
				} else if acquire {
					kind = "acquire"
				}
				problems = append(problems, fmt.Sprintf(
					"%s (%s) mixes transitively reachable native %s operation %q with guest %s lock %s",
					function.Name, filepath.Base(function.File), lock.Device, native, lock.Device, kind))
			}
		}
	}

	if len(problems) != 0 {
		for _, problem := range problems {
			fmt.Fprintln(os.Stderr, "  - "+problem)
		}
		fatalf("failed with %d ownership violation(s)", len(problems))
	}

	fmt.Printf("Runtime-boundary audit passed: %d required symbols, %d generated functions, %d hardware lock families\n",
		len(config.RequiredSymbols), len(functions), len(config.HardwareLocks))
}
