#!/usr/bin/env python3
"""Lists code that only Metrowerks gets: #if/#ifdef __MWERKS__ blocks holding asm and no #else, #else branches that
define a function-like macro as nothing (upstream's fallbacks for an editor's parser), and #else branches that put a
literal where Metrowerks takes an offsetof (a list's link, which GCC lays out elsewhere). GCC compiles each without
the Metrowerks part, so a function can come out empty, read registers never set, or a list overwrite a vtable.

usage: mwerks-only-asm.py PETARI_DIR GENERATED_DIR [Game JSystem nw4r RVL_SDK/gx RVL_SDK/gd RVL_SDK/aralt ...]
Each file is read as the build reads it: the port's compat header or the generated copy where one exists.
"""
import re, sys
from pathlib import Path

petari, generated, *trees = sys.argv[1:]
petari, generated = Path(petari), Path(generated)
condition = re.compile(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")


empty_macro = re.compile(r"\s*#\s*define\s+\w+\([^)]*\)\s*$")


def blocks(text):
    """(line, has_else) for each __MWERKS__ block holding asm; an empty macro fallback counts as one with no #else."""
    stack, found = [], []
    for number, line in enumerate(text.splitlines(), 1):
        match = condition.match(line.strip())
        if match:
            keyword, rest = match.groups()
            if keyword in ("if", "ifdef", "ifndef"):
                mwerks = "__MWERKS__" in rest and keyword != "ifndef" and "!" not in rest
                stack.append({"line": number, "mwerks": mwerks, "asm": False, "else": False, "offsetof": False})
            elif keyword in ("elif", "else") and stack:
                stack[-1]["else"] = True
            elif keyword == "endif" and stack:
                block = stack.pop()
                if block["mwerks"] and block["asm"]:
                    found.append((block["line"], block["else"]))
        elif stack and stack[-1]["mwerks"] and not stack[-1]["else"] and re.search(r"\basm\b", line):
            stack[-1]["asm"] = True
        elif stack and stack[-1]["mwerks"] and not stack[-1]["else"] and "offsetof" in line:
            stack[-1]["offsetof"] = True
        elif stack and stack[-1]["mwerks"] and stack[-1]["else"] and stack[-1]["offsetof"] and "offsetof" not in line \
                and re.search(r"<[^<>]*,\s*-?\d+\s*>", line):
            found.append((number, False))
        elif stack and stack[-1]["mwerks"] and stack[-1]["else"] and empty_macro.match(line):
            found.append((number, False))
    return found


sources = []
for tree in trees:
    for source in sorted(list((petari / "src" / tree).rglob("*.cpp")) + list((petari / "src" / tree).rglob("*.c"))):
        relative = source.relative_to(petari / "src")
        copy = generated / relative
        sources.append((str(relative), copy if copy.exists() else source))
headers = [(str(h.relative_to(petari)), h) for h in sorted(petari.glob("include/**/*.h*"))] + \
          [(str(h.relative_to(petari)), h) for lib in ("JSystem", "nw4r", "RVL_SDK") for h in sorted((petari / "libs" / lib / "include").rglob("*.h*"))]
compat = Path(__file__).resolve().parent.parent / "compat"
for relative, path in headers:
    inner = Path(relative).relative_to(Path(relative).parts[0] if relative.startswith("include/") else Path(*Path(relative).parts[:3]))
    if (compat / inner).exists():
        continue  # the port's compat header is found first and replaces this one
    copy = generated / "include" / inner
    sources.append((relative, copy if copy.exists() else path))
total = 0
for relative, path in sources:
    for line, has_else in blocks(path.read_text(errors="ignore")):
        if not has_else:
            total += 1
            print(f"{relative}:{line}")
print(f"{total} places only Metrowerks compiles", file=sys.stderr)
