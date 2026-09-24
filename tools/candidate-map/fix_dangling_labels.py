#!/usr/bin/env python3
"""Close dangling `goto after_N` in generated code.

N64Recomp emits `goto after_N;` for a call in a branch delay slot and places
`after_N:` on the instruction that follows. When that instruction is past the
end of the function the label is never emitted and the C does not compile with
"use of undeclared label".

Two ways that happens, and neither is fixable from the symbol map:

  * a static function the recompiler inferred over data, whose last word
    decodes as `bgezall` or similar -- the map never mentioned the function, so
    nothing can correct its extent;
  * a partial body left behind by a static that failed to recompile.

Both are already-broken functions over data; they trap before reaching the
label. Appending the missing labels only makes the file compile. A declared
function with this problem is a real boundary bug and is reported instead of
patched, because there the symbol map is the right place to fix it.
"""
import pathlib
import re
import sys

FUNC_RE = re.compile(r'(?m)^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{')


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "generated")
    patched = problems = 0
    for path in sorted(root.glob("*.c")):
        text = path.read_text()
        starts = [(m.start(), m.group(1)) for m in FUNC_RE.finditer(text)]
        if not starts:
            continue
        out, changed = text, False
        for i, (pos, name) in reversed(list(enumerate(starts))):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(out)
            body = out[pos:end]
            missing = (set(re.findall(r'goto (after_\d+);', body))
                       - set(re.findall(r'(after_\d+):', body)))
            if not missing:
                continue
            if not name.startswith("static_"):
                print(f"  {path.name}: {name} needs {sorted(missing)} -- declared "
                      f"function, fix its extent in the symbol map instead",
                      file=sys.stderr)
                problems += 1
                continue
            close = body.rfind(";}")
            if close < 0:
                problems += 1
                continue
            labels = "".join(f"    {label}:\n" for label in sorted(missing))
            out = out[:pos] + body[:close] + labels + body[close:] + out[end:]
            changed = True
            patched += 1
        if changed:
            path.write_text(out)
    print(f"closed {patched} dangling label(s)"
          + (f", {problems} left for the symbol map" if problems else ""))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
