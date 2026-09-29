#!/usr/bin/env python3
"""Apply narrow GCC syntax fixes to generated copies, preserving the upstream checkout."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])
source = root / "src/Game/Util/JMapInfo.cpp"
destination = output / "Game/Util/JMapInfo.cpp"
text = source.read_text()
fixes = [
    ("case JMAP_VALUE_TYPE_STRING_PTR:\n", "case JMAP_VALUE_TYPE_STRING_PTR: {\n"),
    ("*pValueOut = pStringTable + *reinterpret_cast< const u32* >(valuePtr);\n        break;",
     "*pValueOut = pStringTable + *reinterpret_cast< const u32* >(valuePtr);\n        break;\n    }"),
    ("if (item->mShift != 0) {\n        goto FAIL;\n    }",
     "if (item->mShift != 0) {\n        return false;\n    }"),
]
for before, after in fixes:
    if text.count(before) != 1:
        raise SystemExit(f"Upstream syntax changed; review GCC adaptation: {before!r}")
    text = text.replace(before, after)
destination.parent.mkdir(parents=True, exist_ok=True)
text = "// Adapted from Petari: switch/goto scope fixes for GCC; no logic changes.\n" + text
if not destination.exists() or destination.read_text() != text:
    destination.write_text(text)

for relative_source, relative_output in [
    ("include/Game/Util/StringUtil.hpp", "Game/Util/StringUtil.hpp"),
    ("libs/JSystem/include/JSystem/JGadget/hashcode.hpp", "JSystem/JGadget/hashcode.hpp"),
]:
    text = (root / relative_source).read_text()
    if text.count("#include <revolution.h>") != 1:
        raise SystemExit(f"Upstream includes changed: {relative_source}")
    text = text.replace("#include <revolution.h>", "#include <revolution/types.h>")
    destination = output / "include" / relative_output
    destination.parent.mkdir(parents=True, exist_ok=True)
    text = "// Adapted from Petari: include only the types required by these declarations.\n" + text
    if not destination.exists() or destination.read_text() != text:
        destination.write_text(text)

source = root / "include/Game/Util/JMapInfo.hpp"
destination = output / "include/Game/Util/JMapInfo.hpp"
text = source.read_text()
fixes = [
    ("#include <revolution.h>", "#include <revolution/types.h>"),
    ("JMapInfoIter JMapInfo::begin() const {", "inline JMapInfoIter JMapInfo::begin() const {"),
    ("JMapInfoIter JMapInfo::end() const {", "inline JMapInfoIter JMapInfo::end() const {"),
    ("const bool getValue(", "bool getValue("),
    ("const bool JMapInfo::getValue(", "bool JMapInfo::getValue("),
]
for before, after in fixes:
    if text.count(before) != 1:
        raise SystemExit(f"Upstream header changed; review GCC adaptation: {before!r}")
    text = text.replace(before, after)
destination.parent.mkdir(parents=True, exist_ok=True)
text = "// Adapted from Petari: minimal type include and standard C++ inline/return declarations.\n" + text
if not destination.exists() or destination.read_text() != text:
    destination.write_text(text)
