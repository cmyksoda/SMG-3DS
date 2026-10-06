#!/usr/bin/env python3
"""Apply narrow GCC syntax fixes to generated copies, preserving the upstream checkout."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])
produced = set()  # every generated file this run writes; the rest of `output` is stale and removed at the end


def emit(destination, text):
    produced.add(destination.resolve())
    if not destination.exists() or destination.read_text() != text:
        destination.write_text(text)


source = root / "src/Game/Util/JMapInfo.cpp"
destination = output / "Game/Util/JMapInfo.cpp"
text = source.read_text()
fixes = [
    ("case JMAP_VALUE_TYPE_STRING_PTR:\n", "case JMAP_VALUE_TYPE_STRING_PTR: {\n"),
    # Tables come big-endian from the disc; attaching turns one native in place (compat/smg/NativeResources.hpp).
    ('#include <JSystem/JGadget/hashcode.hpp>\n', '#include <JSystem/JGadget/hashcode.hpp>\n#include <smg/NativeResources.hpp>\n'),
    ("    mData = static_cast< const JMapData* >(pData);", "    mData = static_cast< const JMapData* >(smgNativeBcsv(const_cast< void* >(pData)));"),
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
text = "// Adapted from Petari: switch/goto scope fixes for GCC, and tables turned native as they attach.\n" + text
emit(destination, text)


def definition(text, relative, first):
    """The span of the whole definition that starts on the line holding `first`."""
    if text.count(first) != 1:
        raise SystemExit(f"Upstream changed; review {relative}: {first!r}")
    start = text.rfind("\n", 0, text.index(first)) + 1
    depth, end = 0, text.index("{", text.index(first))
    while True:
        depth += {"{": 1, "}": -1}.get(text[end], 0)
        end += 1
        if depth == 0:
            return start, end


# Definitions the port owns natively, left out of the originals' copies so the whole-game library never defines
# them twice: (reason, first lines) per source. The native scene's stand-ins (library smg_native_scene) are never
# linked with the whole game, which keeps its own scene functions.
NATIVE = {
    # Plain new and delete also serve allocations made before the game's heaps exist, and memory outside them.
    "src/JSystem/JKernel/JKRHeap.cpp": ("the port's (source/compat/GameHeap.cpp)", [
        "void* operator new(size_t size) {", "void* operator new[](size_t size) {", "void operator delete(void* pData) {",
        "void operator delete[](void* pData) {"]),
}
written = {}  # source -> (target, text) for each adapted copy, which remainders build on
extracted = {}  # source -> first lines copied into extracts
extracted_fixes = {}  # source -> fixes that hold for the rest of the file too


def drop_definitions(text, relative, firsts, reason):
    for first in firsts:
        start, end = definition(text, relative, first)
        indent = text[start:text.index(first)] + first[:len(first) - len(first.lstrip())]
        text = text[:start] + f"{indent}// Left out, {reason}: {first.strip().rstrip(' {')}" + text[end:]
    return text


def msl_binders(text):
    """MSL's binder2nd names the bound argument's type as a second parameter; the standard one reads it from the
    function object, and every use in the game binds exactly that type, so the second parameter goes."""
    start = 0
    while (start := text.find("std::binder2nd<", start)) != -1:
        depth, position, comma = 0, start + len("std::binder2nd"), None
        while True:
            character = text[position]
            depth += {"<": 1, ">": -1}.get(character, 0)
            if character == "," and depth == 1:
                comma = position
            if depth == 0:
                break
            position += 1
        if comma is not None:
            text = text[:comma] + " " + text[position:]
        start += 1
    return text


def scoped_case(text, relative, label):
    """Metrowerks let a case's declarations reach past the next label; braces give the case its own scope. `label`
    is the label's whole line, or (line, which) when the same line opens cases in several switches."""
    label, which = label if isinstance(label, tuple) else (label, 0)
    line = label + "\n"
    if text.count(line) <= which:
        raise SystemExit(f"Upstream changed; review {relative}: {label!r}")
    start = -1
    for _ in range(which + 1):
        start = text.index(line, start + 1)
    body = start + len(line)
    indent = len(label) - len(label.lstrip())
    end = body
    while True:  # the case runs to the next line indented no deeper than its label: a label or the switch's brace
        following = text[end:text.index("\n", end) + 1]
        if following.strip() and len(following) - len(following.lstrip()) <= indent:
            break
        end += len(following)
    return text[:body - 1] + " {\n" + text[body:end] + " " * indent + "}\n" + text[end:]


def adapt(relative, target, replacements, drop=(), reason="", cases=()):
    """Copy a file with exact replacements; `drop` leaves out whole definitions, found by first line, for `reason`;
    `cases` gives case labels their own scope (scoped_case)."""
    if relative in written:
        raise SystemExit(f"{relative} is adapted twice; the second would replace the first: merge them")
    text = (root / relative).read_text()
    for before, after, expected in replacements:
        if text.count(before) != expected:
            raise SystemExit(f"Upstream changed; review {relative}: {before!r}")
        text = text.replace(before, after)
    for label in cases:
        text = scoped_case(text, relative, label)
    text = msl_binders(text)
    text = drop_definitions(text, relative, drop, reason)
    if relative in NATIVE:
        text = drop_definitions(text, relative, *reversed(NATIVE[relative]))
    written[relative] = (target, text)
    destination = output / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    text = "// Generated Petari adaptation; see tools/prepare-petari.py.\n" + text
    emit(destination, text)


def extract(relative, target, prologue, first_lines, epilogue="", fixes=(), rest_fixes=()):
    """Copy whole function definitions, found by their first line, out of a file too entangled to compile.

    A (namespace, first line) pair copies a definition that sits inside that namespace's block ("" for an
    anonymous one); `fixes` are exact (before, after) replacements made in the copied text; `rest_fixes` also hold
    for the rest of the file, which the whole-game library builds."""
    text = (root / relative).read_text()
    extracted.setdefault(relative, []).extend(first if isinstance(first, str) else first[1] for first in first_lines)
    extracted_fixes.setdefault(relative, []).extend(rest_fixes)
    bodies = []
    for first in first_lines:
        space, first = first if isinstance(first, tuple) else (None, first)
        start, end = definition(text, relative, first)
        bodies.append(f"namespace {space} {{\n{text[start:end]}\n}}  // namespace {space}" if space is not None else text[start:end])
    for fix in fixes:
        before, after, expected = fix if len(fix) == 3 else (*fix, 1)
        joined = "\n\n".join(bodies)
        if joined.count(before) != expected:
            raise SystemExit(f"Upstream changed; review {relative}: {before!r}")
        bodies = [body.replace(before, after) for body in bodies]
    text = (f"// Generated from Petari's {relative}: only these functions; see tools/prepare-petari.py.\n"
            + prologue + "\n" + "\n\n".join(bodies) + "\n" + epilogue)
    destination = output / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    emit(destination, text)


adapt("libs/JSystem/include/JSystem/JMath/JMath.hpp", "include/JSystem/JMath/JMath.hpp", [
    ("#include <revolution/types.h>", "#include <revolution/types.h>\n#include <cmath>\n#include <cstring>", 1),
    ("        return input;\n    }\n#endif", "        return input;\n    }\n#else\n    return input > 0.0f ? std::sqrt(input) : input;\n#endif", 1),
    ("    return ff25;\n#endif", """    return ff25;
#else
    const f32 dt = p5 - p2;
    const f32 t = (p1 - p2) / dt;
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    return (2*t3 - 3*t2 + 1)*p3 + (t3 - 2*t2 + t)*dt*p4
         + (-2*t3 + 3*t2)*p6 + (t3 - t2)*dt*p7;
#endif""", 1),
    ("            stfs y, 8(dest)\n        }\n#endif", "            stfs y, 8(dest)\n        }\n#else\n        std::memmove(dest, src, 3*sizeof(f32));\n#endif", 1),
    ("            psq_st z, 16(dest), 0, 0\n        }\n#endif", "            psq_st z, 16(dest), 0, 0\n        }\n#else\n        std::memmove(dest, src, 6*sizeof(f32));\n#endif", 1),
    ("                psq_st    f_5, 0x28(pDest), 0, 0\n        }\n        ;\n#endif", "                psq_st    f_5, 0x28(pDest), 0, 0\n        }\n        ;\n#else\n        std::memmove(pDest, pSrc, 12*sizeof(f32));\n#endif", 1),
    ("                psq_st    f_7, 0x38(pDest), 0, 0\n        }\n        ;\n#endif", "                psq_st    f_7, 0x38(pDest), 0, 0\n        }\n        ;\n#else\n        std::memmove(pDest, pSrc, 16*sizeof(f32));\n#endif", 1),
    # The game's vector arithmetic, inline as Metrowerks had it (a call each was a few percent of the game's thread).
    ("""#else
    void PSVECCopy(const Vec*, Vec*);
    void PSVECAdd(const Vec*, const Vec*, Vec*);
    void PSVECSubtract(const Vec*, const Vec*, Vec*);
    f32 PSVECDotProduct(const Vec*, const Vec*);
    f32 PSVECSquareMag(const Vec*);
    void PSVECNegate(const Vec*, Vec*);
    f32 PSVECSquareDistance(const Vec*, const Vec*);
    void PSVECMultiply(const Vec*, const Vec*, Vec*);
#endif""", """#else
    inline void PSVECCopy(const Vec* a, Vec* out) { *out = *a; }
    inline void PSVECAdd(const Vec* a, const Vec* b, Vec* out) { *out = {a->x + b->x, a->y + b->y, a->z + b->z}; }
    inline void PSVECSubtract(const Vec* a, const Vec* b, Vec* out) { *out = {a->x - b->x, a->y - b->y, a->z - b->z}; }
    inline void PSVECMultiply(const Vec* a, const Vec* b, Vec* out) { *out = {a->x * b->x, a->y * b->y, a->z * b->z}; }
    inline f32 PSVECDotProduct(const Vec* a, const Vec* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
    inline f32 PSVECSquareMag(const Vec* a) { return JMathInlineVEC::PSVECDotProduct(a, a); }
    inline void PSVECNegate(const Vec* a, Vec* out) { *out = {-a->x, -a->y, -a->z}; }
    inline f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
        const Vec delta{a->x - b->x, a->y - b->y, a->z - b->z};
        return JMathInlineVEC::PSVECSquareMag(&delta);
    }
#endif""", 1),
])

adapt("include/math_types.hpp", "include/math_types.hpp", [
    ("namespace std {\n    inline f32 atan2(f32 x, f32 y) {\n        return ::atan2(x, y);\n    }\n};  // namespace std", "// Use the standard library's float atan2 overload.", 1),
])

adapt("libs/JSystem/include/JSystem/JMath/JMATrigonometric.hpp", "include/JSystem/JMath/JMATrigonometric.hpp", [
    ("namespace std {", "namespace JMath {", 1),
    ("struct pair {", "struct TablePair {", 1),
    ("        pair() {", "        TablePair() {", 1),
    ("}  // namespace std", "}  // namespace JMath", 1),
    ("std::pair< T, T >", "TablePair< T, T >", 1),
    ("v & LEN - 1", "v & (LEN - 1)", 2),
    ("inline f32 JMAAcosRadian(f32 v) NO_INLINE", "NO_INLINE inline f32 JMAAcosRadian(f32 v)", 1),
    ("inline f32 JMAAsinRadian(f32 v) NO_INLINE", "NO_INLINE inline f32 JMAAsinRadian(f32 v)", 1),
    ("    extern TSinCosTable< 14, f32 >", "    template <> f32 TAtanTable<1024, f32>::atan2_(f32, f32) const;\n\n    extern TSinCosTable< 14, f32 >", 1),
])

adapt("src/JSystem/JMath/JMATrigonometric.cpp", "JSystem/JMath/JMATrigonometric.cpp", [
    ("        return mTable[static_cast< s32 >(0.5f + Len * y / x)];", "        const s32 index = static_cast<s32>(0.5f + Len * y / x);\n        return index == Len ? _1000 : mTable[index];", 1),
])

adapt("libs/JSystem/include/JSystem/JGeometry/TUtil.hpp", "include/JSystem/JGeometry/TUtil.hpp", [
    ("#include <cmath>", "#include <cmath>\n#include <cfloat>", 1),
    ("            float outVal = __frsqrte(val);\n            return (val * (0.5f * outVal * (3.0f - (val * (outVal * outVal)))));", "            return std::sqrt(val);", 2),
    ("            f32 outVal = (f32)__frsqrte(val);\n            return outVal * 0.5f * (3.0f - outVal * outVal * val);", "            return 1.0f / std::sqrt(val);", 1),
])

adapt("libs/JSystem/include/JSystem/JGeometry/TVec.hpp", "include/JSystem/JGeometry/TVec.hpp", [
    ("TVec3< T >(", "TVec3(", 2),
    ("        TVec3(int x, int y, int Z);\n", "", 1),
    ("TVec2< f32 >::TVec2()", "inline TVec2< f32 >::TVec2()", 1),
    ("void set(const JGeometry::TVec2< A >& rSrc) NO_INLINE", "NO_INLINE void set(const JGeometry::TVec2< A >& rSrc)", 1),
    ("void set(T _x, T _y, T _z) NO_INLINE", "NO_INLINE void set(T _x, T _y, T _z)", 1),
    ("void setAll(f32 value) NO_INLINE", "NO_INLINE void setAll(f32 value)", 1),
    ("void set(A _x, A _y, A _z, A _w) NO_INLINE", "NO_INLINE void set(A _x, A _y, A _z, A _w)", 1),
    ("                set< f32 >(0.0f, 0.0f, 0.0f, 1.0f);", "                this->template set< f32 >(0.0f, 0.0f, 0.0f, 1.0f);", 1),
    ("inline void negateInternal(const f32* rSrc, f32* rDest);", "inline void negateInternal(const f32* rSrc, f32* rDest) {\n        const f32 x = -rSrc[0], y = -rSrc[1], z = -rSrc[2];\n        rDest[0] = x; rDest[1] = y; rDest[2] = z;\n    }", 1),
    ("static void subInternal(const f32* vec1, const f32* vec2, f32* dst);", "void subInternal(const f32* vec1, const f32* vec2, f32* dst); // Unused; not implemented on ARM.", 1),
    ("void mulInternal(const f32* vec1, const f32* vec2, f32* dst);", "inline void mulInternal(const f32* a, const f32* b, f32* dst) {\n        const f32 x = a[0]*b[0], y = a[1]*b[1], z = a[2]*b[2];\n        dst[0] = x; dst[1] = y; dst[2] = z;\n    }", 1),
    # These return their own local: Metrowerks left it intact on the stack, GCC returns null.
    ("inline TVec2& addInline", "inline TVec2 addInline", 1),
    ("inline const TVec2& addOperatorInline", "inline TVec2 addOperatorInline", 1),
    ("inline TVec2& subInline", "inline TVec2 subInline", 1),
    ("inline const TVec2& subOperatorInline", "inline TVec2 subOperatorInline", 1),
    ("inline TVec2& scaleInline", "inline TVec2 scaleInline", 1),
    ("        TVec3(const Vec& vec);", "        TVec3(const Vec& vec) : Vec(vec) {}", 1),
    ("        TVec3(const TVec3< f32 >& vec);", "        TVec3(const TVec3< f32 >& vec) : Vec(vec) {}", 1),
    ("            stfs b_x, 8(v_b)\n            }\n            ;\n#endif", "            stfs b_x, 8(v_b)\n            }\n            ;\n#else\n            static_cast<Vec&>(*this) = static_cast<const Vec&>(vec);\n#endif", 1),
    ("        void setPSZeroVec();", "        void setPSZeroVec() { zero(); }", 1),
    ("            return _fp1;\n#endif", "            return _fp1;\n#else\n            return JMathInlineVEC::PSVECDotProduct(this, &rOther);\n#endif", 1),
    ("void scaleAdd(register f32 scale", "void scaleAdd(f32 scale", 1),
    ("        f32 squared(const TVec3& rB) const;", "        f32 squared(const TVec3& rB) const { return JMathInlineVEC::PSVECSquareDistance(this, &rB); }", 1),
    ("__fabsf(JMAATan2", "std::fabs(JMAATan2", 1),
    ("                    f32 estimate = __frsqrte(length);\n                    inverse = (0.5f * estimate) * (3.0f - length * (estimate * estimate));", "                    inverse = 1.0f / std::sqrt(length);", 1),
    # The same floats by row and column: indexing past a row's four is undefined to gcc, which may drop the reads.
    ("set< f32 >((*mtx)[3], (*mtx)[7], (*mtx)[11]);", "set< f32 >(mtx[0][3], mtx[1][3], mtx[2][3]);", 1),
])

adapt("libs/JSystem/include/JSystem/JGeometry/TMatrix.hpp", "include/JSystem/JGeometry/TMatrix.hpp", [
    ("rSrc.x * get(0, 0) + rSrc.y * get(0, 1) + rSrc.z * get(0, 2)",
     "rSrc.x * this->get(0, 0) + rSrc.y * this->get(0, 1) + rSrc.z * this->get(0, 2)", 1),
    ("rSrc.x * get(1, 0) + rSrc.y * get(1, 1) + rSrc.z * get(1, 2)",
     "rSrc.x * this->get(1, 0) + rSrc.y * this->get(1, 1) + rSrc.z * this->get(1, 2)", 1),
    ("rSrc.x * get(2, 0) + rSrc.y * get(2, 1) + rSrc.z * get(2, 2)",
     "rSrc.x * this->get(2, 0) + rSrc.y * this->get(2, 1) + rSrc.z * this->get(2, 2)", 1),
])

adapt("libs/JSystem/include/JSystem/JKernel/JKRHeap.hpp", "include/JSystem/JKernel/JKRHeap.hpp", [
    ("""#ifdef __MWERKS__
void* operator new(u32, int);
void* operator new(u32, JKRHeap*);
void* operator new(u32, JKRHeap*, int);
void* operator new[](u32, int);

void* operator new[](u32, JKRHeap*, int);
#endif""", """void* operator new(std::size_t, int);
void* operator new(std::size_t, JKRHeap*);
void* operator new(std::size_t, JKRHeap*, int);
void* operator new[](std::size_t, int);
void* operator new[](std::size_t, JKRHeap*, int);""", 1),
    # A mutex is all it uses from the SDK; the umbrella defines the GX FIFO in every includer.
    ("#include <revolution.h>", "#include <revolution/os/OSMutex.h>\n#include <cstddef>", 1),
])

adapt("libs/JSystem/include/JSystem/JSupport/JSUList.hpp", "include/JSystem/JSupport/JSUList.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>\n#include <cstddef>", 1),
])

# The base's velocity bound a reference to a temporary, which GCC returns as null (no galaxy builds the base alone).
adapt("include/Game/MapObj/FloaterFloatingForce.hpp", "include/Game/MapObj/FloaterFloatingForce.hpp", [
    ("        return TVec3f(0.0f, 0.0f, 0.0f);", "        static const TVec3f sZero(0.0f, 0.0f, 0.0f);\n        return sZero;", 1),
])
adapt("include/Game/Util/MathUtil.hpp", "include/Game/Util/MathUtil.hpp", [
    ("        return __fabsf(x);", "        return std::fabs(x);", 1),
    ("        return __abs(x);", "        return x < 0 ? -x : x;", 1),
    ("        return __fabs(x);", "        return std::fabs(x);", 1),
    # The Wii's estimates, computed exactly; non-positive input passes through as it does there.
    ("#else\n    f32 frsqrte(f32);\n    f32 fastSqrtf(f32);\n#endif",
     "#else\n    inline f32 frsqrte(f32 x) { return 1.0f / std::sqrt(x); }\n"
     "    inline f32 fastSqrtf(f32 x) { return x > 0.0f ? std::sqrt(x) : x; }\n#endif", 1),
    ("inline void clampMax(f32* pValue, f32 max) NO_INLINE {", "NO_INLINE inline void clampMax(f32* pValue, f32 max) {", 1),
    # Only GXColor is used; the whole GX header defines the Wii's hardware FIFO in every user.
    ("#include <revolution/gx.h>", "#include <revolution/gx/GXStruct.h>", 1),
    # Each directional particle asks it (a quarter of Dusty Dune's game thread is those): inline, the same compares.
    ("    bool isNearZero(const TVec3f& rVec, f32 tolerance = 0.001f);",
     "    inline bool isNearZero(const TVec3f& rVec, f32 tolerance = 0.001f) {\n"
     "        return !(rVec.x > tolerance || rVec.x < -tolerance || rVec.y > tolerance || rVec.y < -tolerance || rVec.z > tolerance ||\n"
     "                 rVec.z < -tolerance);\n    }", 1),
])

adapt("src/Game/Util/MathUtil.cpp", "Game/Util/MathUtil.cpp", [
    ("""    __REGISTER f32 inverse;
    __asm { frsqrte inverse, value }
    f32 estimate = inverse * value;
    inverse = -(estimate * inverse - 3.0f);
    inverse *= estimate;
    inverse *= 0.5f;
    return inverse;""", "    return std::sqrt(value);", 1),
    ('#include "Game/Util/MathUtil.hpp"', '#include "Game/Util/MathUtil.hpp"\n#include <cmath>', 1),
    ('#include "Game/System/GameSystem.hpp"\n#include "Game/System/GameSystemObjHolder.hpp"\n',
     '#include <smg/GameRandom.hpp>\n', 1),
    ('#include "Game/Util/SingletonHolder.hpp"\n', '', 1),
    ('''    bool isNearZero(const TVec3f& rVec, f32 tolerance) {
        if (rVec.x > tolerance) {
            return false;
        }

        if (rVec.x < -tolerance) {
            return false;
        }

        if (rVec.y > tolerance) {
            return false;
        }

        if (rVec.y < -tolerance) {
            return false;
        }

        if (rVec.z > tolerance) {
            return false;
        }

        if (rVec.z < -tolerance) {
            return false;
        }

        return true;
    }

''', '', 1),
    ("SingletonHolder< GameSystem >::get()->mObjHolder->mRandom.rand()", "smgGameRandom().rand()", 1),
    # Paired-single bodies with no other branch; these are what the assembly computes.
    ("""    }  // clang-format on
    return dot;
#endif""", """    }  // clang-format on
    return dot;
#else
    Vec* out = const_cast< Vec* >(pDst);
    const f32 dot = pSrc->x * pKill->x + pSrc->y * pKill->y + pSrc->z * pKill->z;
    const Vec src = *pSrc;
    out->x = src.x - dot * pKill->x;
    out->y = src.y - dot * pKill->y;
    out->z = src.z - dot * pKill->z;
    return dot;
#endif""", 1),
    ("""            psq_st f2, 8(pA1), 1, 0
        }

#endif""", """            psq_st f2, 8(pA1), 1, 0
        }
#else
        TVec3f* out = const_cast< TVec3f* >(pA1);
        out->x += pA2->x * a3;
        out->y += pA2->y * a3;
        out->z += pA2->z * a3;
#endif""", 1),
    ("""            psq_st    f3, 8(pA3), 1, 0
        }

#endif""", """            psq_st    f3, 8(pA3), 1, 0
        }
#else
        const TVec3f a = *pA1, b = *pA2;
        pA3->x = a.x * a4 + b.x * a5;
        pA3->y = a.y * a4 + b.y * a5;
        pA3->z = a.z * a4 + b.z * a5;
#endif""", 1),
])

adapt("src/Game/Util/MtxUtil.cpp", "Game/Util/MtxUtil.cpp", [])

adapt("include/Game/Util/Array.hpp", "include/Game/Util/Array.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

adapt("libs/JSystem/include/JSystem/JMath/random.hpp", "include/JSystem/JMath/random.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

adapt("libs/JSystem/include/JSystem/JGeometry/TBox.hpp", "include/JSystem/JGeometry/TBox.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

# Only matrix types are needed; the whole SDK brings display-list globals into every user.
adapt("include/Game/Util/MtxUtil.hpp", "include/Game/Util/MtxUtil.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

# The gravity shapes use only math helpers; the umbrella headers drag in layouts, text and actors.
for name in ["PlanetGravity", "PointGravity", "ParallelGravity", "CubeGravity", "DiskGravity",
             "DiskTorusGravity", "ConeGravity", "SegmentGravity", "WireGravity"]:
    relative = f"src/Game/Gravity/{name}.cpp"
    text = (root / relative).read_text()
    adapt(relative, f"Game/Gravity/{name}.cpp", [
        ('#include "Game/Util.hpp"', '#include "Game/Util/MathUtil.hpp"', text.count('#include "Game/Util.hpp"')),
        # The umbrella also brings the actor-based gravity objects, and with them most of the game.
        ('#include "Game/Gravity.hpp"', f'#include "Game/Gravity/{name}.hpp"', text.count('#include "Game/Gravity.hpp"')),
    ])

adapt("include/Game/NameObj/NameObjHolder.hpp", "include/Game/NameObj/NameObjHolder.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

adapt("include/Game/Util/ObjUtil.hpp", "include/Game/Util/ObjUtil.hpp", [
    ("#include <revolution/gx.h>", "#include <revolution/gx/GXStruct.h>", 1),
])

# std::mem_fun left the standard library in C++17; std::mem_fn calls the member the same way.
adapt("src/Game/NameObj/NameObjHolder.cpp", "Game/NameObj/NameObjHolder.cpp", [
    ("std::mem_fun(pMethod)", "std::mem_fn(pMethod)", 1),
    ("#include <algorithm>", "#include <algorithm>\n#include <functional>", 1),
])

adapt("src/Game/Util/HashUtil.cpp", "Game/Util/HashUtil.cpp", [
    # MSL's "C" locale table lowers only ASCII letters; every other byte, Shift-JIS included, passes through.
    ("#include <locale.h>\n\n// needed here for inlining reasons\ninline int tolower(int c) {\n"
     "    return ((c < 0) || (c >= 0x100)) ? c : (int)(_current_locale.ctype_cmpt_ptr->lower_map_ptr[c]);\n}",
     "static int lowerAscii(int c) {\n    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;\n}", 1),
    ("hash = tolower(*pStr) + hash * 31;", "hash = lowerAscii(*pStr) + hash * 31;", 1),
])

# Every list calls its delegator through the non-const type, const lists too; a shared base makes that call defined.
adapt("include/Game/NameObj/NameObjCategoryList.hpp", "include/Game/NameObj/NameObjCategoryList.hpp", [
    ("namespace {\n    template < typename T >\n    class NameObjRealDelegator {",
     "namespace {\n    class NameObjDelegator {\n    public:\n        virtual ~NameObjDelegator() {}\n"
     "        virtual void operator()(NameObj* pNameObj) = 0;\n    };\n\n"
     "    template < typename T >\n    class NameObjRealDelegator : public NameObjDelegator {", 1),
    ("    /* 0x08 */ union {\n        NameObjRealDelegator< NameObjMethod >* mDelegator;\n"
     "        NameObjRealDelegator< NameObjMethodConst >* mDelegatorConst;\n    };",
     "    /* 0x08 */ NameObjDelegator* mDelegator;", 1),
])
adapt("src/Game/NameObj/NameObjCategoryList.cpp", "Game/NameObj/NameObjCategoryList.cpp", [
    ("mDelegatorConst = new NameObjRealDelegator< NameObjMethodConst >(method);",
     "mDelegator = new NameObjRealDelegator< NameObjMethodConst >(method);", 1),
])

# ObjUtil and SystemUtil reach most of the game; NameObj needs only these.
extract("src/Game/Util/ObjUtil.cpp", "Game/Util/ObjUtilNameObj.cpp",
        '#include "Game/Util/ObjUtil.hpp"\n#include "Game/NameObj/NameObj.hpp"\n',
        ["void MR::requestMovementOn(NameObj* pObj) {", "void MR::requestMovementOff(NameObj* pObj) {"])
extract("src/Game/Util/SystemUtil.cpp", "Game/Util/SystemUtilSceneNameObj.cpp",
        '#include "Game/Util/SystemUtil.hpp"\n#include "Game/NameObj/NameObjHolder.hpp"\n\n'
        "// Held by GameSystemSceneController in the game; answered by the native scene (source/compat/Scene.cpp).\n"
        "NameObjHolder* getSceneNameObjHolder();\n\nnamespace MR {",
        ["void callMethodAllSceneNameObj(NameObjMethod pMethod) {", "void suspendAllSceneNameObj() {",
         "void resumeAllSceneNameObj() {", "void syncWithFlagsAllSceneNameObj() {"], "};  // namespace MR\n")

# Hosts are compared as u32, which holds a pointer on the Wii and the 3DS; 64-bit host tests compare the low half.
adapt("src/Game/Gravity/PlanetGravityManager.cpp", "Game/Gravity/PlanetGravityManager.cpp", [
    ('#include "Game/Gravity.hpp"', '#include "Game/Gravity/GravityInfo.hpp"\n#include "Game/Gravity/PlanetGravityManager.hpp"\n#include <cstdint>', 1),
    ("(host != (u32)(*pGravities)->mHost)", "(host != (u32)(uintptr_t)(*pGravities)->mHost)", 1),
])

adapt("src/Game/Gravity/GravityInfo.cpp", "Game/Gravity/GravityInfo.cpp", [
    ('#include "Game/Gravity.hpp"', '#include "Game/Gravity/GravityInfo.hpp"', 1),
])

adapt("include/Game/Gravity/GravityInfo.hpp", "include/Game/Gravity/GravityInfo.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

# SceneObjHolder's own methods; the objects it makes, and where it lives, are the native scene's (source/compat/Scene.cpp).
extract("src/Game/Scene/SceneObjHolder.cpp", "Game/Scene/SceneObjHolderMethods.cpp",
        '#include "Game/Scene/SceneObjHolder.hpp"\n#include "Game/NameObj/NameObj.hpp"\n',
        ["SceneObjHolder::SceneObjHolder() {", "NameObj* SceneObjHolder::create(int id) {",
         "NameObj* SceneObjHolder::getObj(int id) const {", "bool SceneObjHolder::isExist(int id) const {",
         ("MR", "NameObj* createSceneObj(int id) {")])

# JMapUtil reads placement rows; its umbrella include reaches the whole game, and NO_INLINE only mattered for matching.
adapt("include/Game/Util/JMapUtil.hpp", "include/Game/Util/JMapUtil.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])
adapt("src/Game/Util/JMapUtil.cpp", "Game/Util/JMapUtil.cpp", [
    ('#include "Game/Util.hpp"', '#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/MtxUtil.hpp"\n'
     '#include "Game/Util/SceneUtil.hpp"\n#include "Game/Util/StringUtil.hpp"', 1),
    (") NO_INLINE {", ") {", 5),
])
extract("src/Game/Util/StringUtil.cpp", "Game/Util/StringUtilCompare.cpp",
        '#include "Game/Util/StringUtil.hpp"\n#include <cstring>\n',
        [("MR", "bool isEqualString(const char* pStr1, const char* pStr2) {")])

adapt("src/Game/Util/GravityUtil.cpp", "Game/Util/GravityUtil.cpp", [
    ("#include <cstring>", "#include <cstdint>\n#include <cstring>", 1),
    ("u32 host) NO_INLINE {", "u32 host) {", 1),
    # A u32 holds a pointer on the Wii and the 3DS; 64-bit host tests keep the low half, as the manager compares.
    ("host = (u32)pActor;", "host = (u32)(uintptr_t)pActor;", 1),
])

adapt("src/Game/Gravity/GravityCreator.cpp", "Game/Gravity/GravityCreator.cpp", [
    ('#include "Game/Gravity.hpp"\n#include "Game/Util.hpp"',
     '#include "Game/Gravity/GravityCreator.hpp"\n#include "Game/Util/GravityUtil.hpp"\n#include "Game/Util/JMapUtil.hpp"\n'
     '#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/MtxUtil.hpp"', 1),
])

# Rails: the wire gravity creator samples one through RailRider. Camera rails come with the camera.
adapt("src/Game/LiveActor/RailRider.cpp", "Game/LiveActor/RailRider.cpp", [], drop=[
    "RailRider::RailRider(s32 a1, s32 a2)",
], reason="camera rails come with the camera")

# Collision: the original KCollisionServer and attribute codes. CollisionParts, which owns a server per
# placed part, takes a HitSensor whose host is a LiveActor, so it waits for actors.
adapt("include/Game/Map/KCollision.hpp", "include/Game/Map/KCollision.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
    ("static bool isBinaryInitialized(const void*) NO_INLINE;", "static bool isBinaryInitialized(const void*);", 1),
])
adapt("include/Game/Camera/CameraPolygonCodeUtil.hpp", "include/Game/Camera/CameraPolygonCodeUtil.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])
adapt("src/Game/Map/KCollision.cpp", "Game/Map/KCollision.cpp", [
    ('#include "Game/Map/CollisionDirector.hpp"\n', "", 1),
    ('#include <algorithm>\n', '#include <algorithm>\n#include <cstdint>\n\n'
     "// CollisionDirector's CollisionCode; answered by the native scene (source/compat/Scene.cpp) until the director runs.\n"
     "CollisionCode* getSceneCollisionCode();\n", 1),
    ("MR::getCollisionDirector()->mCode->getCameraID(iter)", "getSceneCollisionCode()->getCameraID(iter)", 1),
    # Shifting by -1 is undefined in C++ (PowerPC's slw gave 0, then overwritten); a single block skips it.
    ("    s32 offset = (((rZ >> blockWidthShift) << xyShift) | ((rY >> blockWidthShift) << xShift) | (rX >> blockWidthShift)) * 4;\n\n"
     "    if (xyShift == -1 && xShift == -1) {\n        offset = 0;\n    }",
     "    s32 offset = 0;\n\n"
     "    if (xyShift != -1 || xShift != -1) {\n"
     "        offset = (((rZ >> blockWidthShift) << xyShift) | ((rY >> blockWidthShift) << xShift) | (rX >> blockWidthShift)) * 4;\n"
     "    }", 1),
    # GCC rejects a goto past an initialized declaration; declare first, then assign.
    ("    f32 distance = MR::sqrt(*pDist);", "    f32 distance;\n    distance = MR::sqrt(*pDist);", 1),
    ("bool KCollisionServer::isBinaryInitialized(const void* pData) NO_INLINE {\n"
     "    return reinterpret_cast< const s32* >(pData)[0] < 0;",
     "bool KCollisionServer::isBinaryInitialized(const void* pData) {\n"
     "    // Wii pointers have the top bit set and file offsets never do; anywhere, a pointer into the image\n"
     "    // is no smaller than the image's own address, which no offset reaches.\n"
     "    return reinterpret_cast< uintptr_t >(static_cast< const KCLFile* >(pData)->mPos) >= reinterpret_cast< uintptr_t >(pData);", 1),
])
# CollisionCode compiles as it is now that Triangle has come with CollisionParts.

# Stage switches: the container, zone switches, switch controls and sleep controllers are the originals.
# The game leaves the zone switches to the scene heap; the native scene frees them, so it needs to see them.
adapt("include/Game/Map/StageSwitch.hpp", "include/Game/Map/StageSwitch.hpp", [
    ("private:\n    /* 0x0C */", "    /* 0x0C */", 1),
])
adapt("include/Game/Util/JMapIdInfo.hpp", "include/Game/Util/JMapIdInfo.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])

# Lights: the original data and zone holders read LightData.arc; the light director that owns them, and
# its archive, are the native scene's (source/compat/Light.cpp).
adapt("include/Game/Map/LightDataHolder.hpp", "include/Game/Map/LightDataHolder.hpp", [
    ("#include <revolution.h>", "#include <revolution/gx/GXStruct.h>\n#include <revolution/mtx.h>", 1),
])
adapt("include/Game/Map/LightZoneDataHolder.hpp", "include/Game/Map/LightZoneDataHolder.hpp", [
    ("#include <revolution.h>", "#include <revolution/types.h>", 1),
])
adapt("src/Game/Map/LightDataHolder.cpp", "Game/Map/LightDataHolder.cpp", [
    ('#include "Game/Util.hpp"', '#include "Game/Util/StringUtil.hpp"', 1),
])
adapt("src/Game/Map/LightZoneDataHolder.cpp", "Game/Map/LightZoneDataHolder.cpp", [
    ('#include "Game/Util.hpp"', '#include "Game/Util/ObjUtil.hpp"\n#include "Game/Util/SceneUtil.hpp"', 1),
])
extract("src/Game/Map/LightFunction.cpp", "Game/Map/LightFunctionData.cpp",
        '#include "Game/Map/LightFunction.hpp"\n#include "Game/Util/JMapInfo.hpp"\n#include "Game/Util/ObjUtil.hpp"\n#include <cstdio>\n',
        [("", "void getDataLightInfo(JMapInfo* pInfo, int id, LightInfo* pLightInfo, const char* pName) {"),
         ("", "void getDataActorLightInfo(JMapInfo* pInfo, int lightID, ActorLightInfo* pActorInfo, const char* pName) {"),
         "void LightFunction::getAreaLightLightData(JMapInfo* pInfo, int idx, AreaLightInfo* pLightInfo) {"])
extract("src/Game/Util/ObjUtil.cpp", "Game/Util/ObjUtilCsv.cpp",
        '#include "Game/Util/ObjUtil.hpp"\n#include "Game/Util/JMapInfo.hpp"\n#include <revolution/gx/GXStruct.h>\n'
        '#include <revolution/mtx.h>\n#include <cstdio>\n',
        ["void MR::getCsvDataStr(const char** pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {",
         "void MR::getCsvDataS32(s32* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {",
         "void MR::getCsvDataU8(u8* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) NO_INLINE {",
         "void MR::getCsvDataF32(f32* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {",
         "void MR::getCsvDataBool(bool* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {",
         "void MR::getCsvDataVec(Vec* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {",
         "void MR::getCsvDataColor(GXColor* pOut, const JMapInfo* pMapInfo, const char* pKey, s32 idx) {"],
        fixes=[(" NO_INLINE {", " {")])

# Areas: the original forms, AreaObj and LightArea. The container, followers, switch watchers and sleep
# control they reach are the native scene's (source/compat/Area.cpp).
# The base form is only ever one of its subclasses and its virtuals have no bodies: pure, as GCC needs them.
adapt("include/Game/AreaObj/AreaForm.hpp", "include/Game/AreaObj/AreaForm.hpp", [
    ("    virtual void init(const JMapInfoIter&);\n    virtual bool isInVolume(const TVec3f&) const;\n\n    TPos3f* _4;",
     "    virtual void init(const JMapInfoIter&) = 0;\n    virtual bool isInVolume(const TVec3f&) const = 0;\n\n    TPos3f* _4;", 1),
])
adapt("src/Game/AreaObj/AreaForm.cpp", "Game/AreaObj/AreaForm.cpp", [
    ('#include "Game/Util.hpp"', '#include "Game/Util/JMapUtil.hpp"\n#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/MtxUtil.hpp"', 1),
])
extract("src/Game/Util/BaseMatrixFollowTargetHolder.cpp", "Game/Util/FollowIdCheck.cpp",
        '#include "Game/Util/BaseMatrixFollowTargetHolder.hpp"\n#include "Game/Util/JMapLinkInfo.hpp"\n#include "Game/Util/JMapUtil.hpp"\n',
        [("MR", "bool isValidFollowID(const JMapInfoIter& rIter) {")])
adapt("src/Game/AreaObj/AreaObj.cpp", "Game/AreaObj/AreaObj.cpp", [
    ("#include <functional.hpp>\n", "", 1),
    # MSL's rfind_if walked down to Begin - 1, a pointer before the array (nullptr - 1 for an empty manager).
    ("""    AreaObj* const* Begin = mArray.begin();
    AreaObj* const* End = mArray.end();
    AreaObj* const* p = std::rfind_if(
        End - 1, Begin - 1,
        std::binder2nd< std::const_mem_fun1_t< bool, AreaObj, const TVec3f& >, const TVec3f& >(std::mem_func(&AreaObj::isInVolume), rVec));

    p = (p == Begin - 1 ? End : p);

    if (p != mArray.end()) {
        return *p;
    }

    return nullptr;""",
     """    for (s32 i = mArray.size() - 1; i >= 0; i--) {
        if (mArray[i]->isInVolume(rVec)) {
            return mArray[i];
        }
    }

    return nullptr;""", 1),
])

# J3D material animation: the original key tables and their interpolation. The loader reads Wii files in
# place; the port's loader (source/core/Animation.cpp) gives these objects native copies instead.
# OSInitFastCast sets the quantized registers to plain integers; psq_st truncates and saturates.
adapt("libs/RVL_SDK/include/revolution/os.h", "include/revolution/os.h", [
    # Wii low-memory addresses, which Metrowerks pins in place; declared here so each is one object or none.
    ("u32 __OSBusClock = 0x800000F8;\nu32 __MEM2End = 0x80003128;", "extern u32 __OSBusClock;\nextern u32 __MEM2End;", 1),
    # The Wii sees memory three ways (cached at 0x80000000, uncached at 0xC0000000, physical from 0); a 3DS app has one
    # address space, whose addresses all fit the GPU's 29-bit physical fields, so each view is the address itself.
    ("#define OSPhysicalToCached(paddr) ((void*)((u32)(paddr) + OS_BASE_CACHED))\n"
     "#define OSCachedToPhysical(caddr) ((u32)((u8*)(caddr)-OS_BASE_CACHED))\n"
     "#define OSUncachedToPhysical(ucaddr) ((u32)((u32)(ucaddr) & (~OS_BASE_UNCACHED)))\n"
     "#define OSPhysicalToUncached(paddr) ((void*)((u32)(paddr) + OS_BASE_UNCACHED))",
     "#define OSPhysicalToCached(paddr) ((void*)(uintptr_t)(u32)(paddr))\n"
     "#define OSCachedToPhysical(caddr) ((u32)(uintptr_t)(caddr))\n"
     "#define OSUncachedToPhysical(ucaddr) ((u32)(uintptr_t)(ucaddr))\n"
     "#define OSPhysicalToUncached(paddr) ((void*)(uintptr_t)(u32)(paddr))", 1),
    ("""        psq_st inF, 0(tmpPtr), 0x1, 5
        lha out, 0(tmpPtr)
    }
#endif""", """        psq_st inF, 0(tmpPtr), 0x1, 5
        lha out, 0(tmpPtr)
    }
#else
    out = static_cast< s16 >(inF < -32768.0f ? -32768.0f : inF > 32767.0f ? 32767.0f : inF);
#endif""", 1),
    ("""        psq_st inF, 0(tmpPtr), 0x1, 2
        lbz out, 0(tmpPtr)
    }
#endif""", """        psq_st inF, 0(tmpPtr), 0x1, 2
        lbz out, 0(tmpPtr)
    }
#else
    out = static_cast< u8 >(inF < 0.0f ? 0.0f : inF > 255.0f ? 255.0f : inF);
#endif""", 1),
])
for header, fix in [("libs/JSystem/include/JSystem/J3DGraphAnimator/J3DAnimation.hpp",
                     "#include <revolution/gx/GXStruct.h>\n#include <revolution/mtx.h>\n#include <cstddef>"),
                    ("libs/JSystem/include/JSystem/J3DGraphBase/J3DStruct.hpp", "#include <revolution/gx/GXStruct.h>\n#include <revolution/mtx.h>\n#include <cstring>"),
                    ("libs/JSystem/include/JSystem/J3DGraphBase/J3DTransform.hpp", "#include <revolution/mtx.h>"),
                    ("libs/JSystem/include/JSystem/JUtility/JUTNameTab.hpp", "#include <revolution/types.h>")]:
    # __memcpy is a Metrowerks intrinsic for the standard function.
    extra = [("__memcpy(", "memcpy(", 5)] if header.endswith("J3DStruct.hpp") else []
    if header.endswith("J3DStruct.hpp"):
        extra.append(("""            psq_st xy, 0(dst), 0, 0
        }
        ;
#endif
    }""", """            psq_st xy, 0(dst), 0, 0
        }
        ;
#else
        mScaleX = other.mScaleX;
        mScaleY = other.mScaleY;
        mRotation = other.mRotation;
        mTranslationX = other.mTranslationX;
        mTranslationY = other.mTranslationY;
#endif
    }""", 1))
    if header.endswith("J3DTransform.hpp"):
        extra.append(("        return *this;\n    }\n#endif\n};", "        return *this;\n    }\n#else\n"
                      "    // GCC's implicit copy assigns every member, which is what the paired singles did.\n#endif\n};", 1))
    adapt(header, header.replace("libs/JSystem/include/", "include/"), [("#include <revolution.h>", fix, 1)] + extra)
extract("src/JSystem/J3DGraphAnimator/J3DAnimation.cpp", "JSystem/J3DGraphAnimator/J3DAnimationKeys.cpp",
        '#include "JSystem/J3DGraphAnimator/J3DAnimation.hpp"\n#include "JSystem/J3DGraphBase/J3DStruct.hpp"\n'
        '#include "JSystem/JMath/JMath.hpp"\n#include <revolution/os.h>\n',
        ["inline f32 J3DHermiteInterpolation(f32 p1, f32 const* p2, f32 const* p3, f32 const* p4, f32 const* p5, f32 const* p6, f32 const* p7) {",
         "inline f32 J3DHermiteInterpolation(__REGISTER f32 pp1, __REGISTER s16 const* pp2, __REGISTER s16 const* pp3, __REGISTER s16 const* pp4,",
         "f32 J3DGetKeyFrameInterpolation(f32 frame, J3DAnmKeyTableBase* pKeyTable, T* pData) {",
         "void J3DFrameCtrl::init(s16 endFrame) {",
         "void J3DFrameCtrl::update() {",
         "J3DAnmTransform::J3DAnmTransform(s16 frameMax, f32* pScaleData, s16* pRotData, f32* pTransData) : J3DAnmBase(frameMax) {",
         "void J3DAnmTransformKey::calcTransform(f32 frame, u16 jointNo, J3DTransformInfo* pTransform) const {",
         "J3DAnmTextureSRTKey::J3DAnmTextureSRTKey() {",
         "void J3DAnmTextureSRTKey::calcTransform(f32 frame, u16 jointNo, J3DTextureSRTInfo* pTexSRTInfo) const {",
         "J3DAnmTevRegKey::J3DAnmTevRegKey() {",
         "void J3DAnmTevRegKey::getTevColorReg(u16 index, GXColorS10* pColor) const {",
         "void J3DAnmTevRegKey::getTevKonstReg(u16 index, GXColor* pColor) const {",
         "void J3DAnmTexPattern::getTexNo(u16 index, u16* pTexNo) const {"],
        fixes=[("    return value;\n#endif\n}",
                "    return value;\n#else\n    return JMAHermiteInterpolation(pp1, *pp2, *pp3, *pp4, *pp5, *pp6, *pp7);\n#endif\n}"),
               ("f32 J3DGetKeyFrameInterpolation(f32 frame", "template < typename T >\nf32 J3DGetKeyFrameInterpolation(f32 frame"),
               # Rotations are often negative; shifting one left is undefined before C++20 (slw just scales it).
               ("<< mDecShift", "* (1 << mDecShift)", 8)],
        rest_fixes=[("    return value;\n#endif\n}",
                     "    return value;\n#else\n    return JMAHermiteInterpolation(pp1, *pp2, *pp3, *pp4, *pp5, *pp6, *pp7);\n#endif\n}"),
                    ("<< mDecShift", "* (1 << mDecShift)")])

adapt("include/Game/System/WPadButton.hpp", "include/Game/System/WPadButton.hpp", [
    ("#include <revolution.h>", "#include <revolution/kpad.h>", 1),
])

adapt("libs/RVL_SDK/include/revolution/hbm/HBMBase.h", "include/revolution/hbm/HBMBase.h", [
    ("#include \"revolution/mem.h\"", "typedef struct MEMAllocator MEMAllocator;", 1),
])

adapt("libs/RVL_SDK/include/revolution/wpad.h", "include/revolution/wpad.h", [
    ('#include "revolution/os.h"', '#include "revolution/os/OSThread.h"\n#include "revolution/os/OSTime.h"', 1),
    ('#include "revolution/sc.h"', '', 1),
    ('static void WPADiShutdown(BOOL);', 'void WPADiShutdown(BOOL);', 1),
])

# PowerPC fabs and fsel as plain C++: fsel takes the first value when cond >= 0, else (and for NaN) the second.
adapt("libs/nw4r/include/nw4r/math/arithmetic.h", "include/nw4r/math/arithmetic.h", [
    ("""            __asm {
                fabs ax, x
            }
            ;""", """#ifdef __MWERKS__
            __asm {
                fabs ax, x
            }
            ;
#else
            ax = __builtin_fabsf(x);
#endif""", 1),
    ("""            asm { fsel   ret, cond, ifPos, ifNeg }""", """#ifdef __MWERKS__
            asm { fsel   ret, cond, ifPos, ifNeg }
#else
            ret = cond >= 0.0f ? ifPos : ifNeg;
#endif""", 1),
])

# A typedef befriends with "friend Self;"; Metrowerks also took the elaborated "friend class Self;".
adapt("libs/nw4r/include/nw4r/ut/LinkList.h", "include/nw4r/ut/LinkList.h", [
    ("friend class Self;", "friend Self;", 1),
])

# The ripper's own inline callers pass u32, which is unsigned long on the Wii and ARM but not on 64-bit hosts.
adapt("libs/JSystem/include/JSystem/JKernel/JKRDvdRipper.hpp", "include/JSystem/JKernel/JKRDvdRipper.hpp", [
    ("""    static void* loadToMainRAM(const char*, unsigned char*, JKRExpandSwitch, unsigned long, JKRHeap*, EAllocDirection, unsigned long, int*,
                               unsigned long*);""",
     """    static void* loadToMainRAM(const char*, unsigned char*, JKRExpandSwitch, u32, JKRHeap*, EAllocDirection, u32, int*, u32*);""", 1),
    ("""    static void* loadToMainRAM(long, unsigned char*, JKRExpandSwitch, unsigned long, JKRHeap*, EAllocDirection, unsigned long, int*, unsigned long*);""",
     """    static void* loadToMainRAM(s32, unsigned char*, JKRExpandSwitch, u32, JKRHeap*, EAllocDirection, u32, int*, u32*);""", 1),
    ("""    static void* loadToMainRAM(JKRDvdFile*, unsigned char*, JKRExpandSwitch, unsigned long, JKRHeap*, EAllocDirection, unsigned long, int*,
                               unsigned long*);""",
     """    static void* loadToMainRAM(JKRDvdFile*, unsigned char*, JKRExpandSwitch, u32, JKRHeap*, EAllocDirection, u32, int*, u32*);""", 1),
])

# A template names its dependent base with its arguments; Metrowerks found the bare name.
adapt("libs/JSystem/include/JSystem/JAudio2/JASGadget.hpp", "include/JSystem/JAudio2/JASGadget.hpp", [
    ("JASPtrArray() : JASPtrTable(mPtrArray, LEN) {", "JASPtrArray() : JASPtrTable< T >(mPtrArray, LEN) {", 1),
])

# JGadget declares the standard iterator tags and std::iterator itself, which Metrowerks' library lacked.
adapt("libs/JSystem/include/JSystem/JGadget/linklist.hpp", "include/JSystem/JGadget/linklist.hpp", [
    ("""namespace std {

    struct input_iterator_tag {};
    struct forward_iterator_tag : public input_iterator_tag {};
    struct bidirectional_iterator_tag : public forward_iterator_tag {};

    template < typename Category, typename T, typename Distance = s32, typename Pointer = T*, typename Reference = T& >
    class iterator {
    public:
        typedef Category iterator_category;
        typedef T value_type;
        typedef Distance difference_type;
        typedef Pointer pointer;
        typedef Reference reference;
        iterator& operator=(const iterator& rOther) NO_INLINE {
            return *this;
        }
    };

}  // namespace std""", "#include <iterator>", 1),
])

# Pointer arithmetic through u32 assumes 32-bit pointers, true on the Wii and the 3DS but not on 64-bit hosts.
adapt("libs/RVL_SDK/include/revolution/mem/heapCommon.h", "include/revolution/mem/heapCommon.h", [
    ("typedef u32 UIntPtr;", "typedef uintptr_t UIntPtr;", 1),
])
adapt("libs/JSystem/include/JSystem/JAudio2/JASSeqReader.hpp", "include/JSystem/JAudio2/JASSeqReader.hpp", [
    ("return (u32)mSeqCursor - (u32)mSeqBuff;", "return static_cast< u32 >(mSeqCursor - mSeqBuff);", 1),
    # Sequence arguments and tables are big-endian in the bytecode, which the Wii's plain loads read as such.
    ("    u32 get24(u32 param_0) const {\n        return (*(u32*)(mSeqBuff + param_0 - 1)) & 0xffffff;\n    }",
     "    u32 get24(u32 param_0) const {\n        const u8* at = mSeqBuff + param_0;\n        return u32(at[0]) << 16 | u32(at[1]) << 8 | at[2];\n    }", 1),
    ("    u16 get16(u32 param_0) const {\n        return *(u16*)(mSeqBuff + param_0);\n    }",
     "    u16 get16(u32 param_0) const {\n        const u8* at = mSeqBuff + param_0;\n        return u16(at[0] << 8 | at[1]);\n    }", 1),
    ("    u32 get32(u32 param_0) const {\n        return *(u32*)(mSeqBuff + param_0);\n    }",
     "    u32 get32(u32 param_0) const {\n        const u8* at = mSeqBuff + param_0;\n        return u32(at[0]) << 24 | u32(at[1]) << 16 | u32(at[2]) << 8 | at[3];\n    }", 1),
    ("#else\n        u16* value = (u16*)mSeqCursor;\n        mSeqCursor += 2;\n        return *value;\n#endif",
     "#else\n        const u32 value = u32(mSeqCursor[0]) << 8 | mSeqCursor[1];\n        mSeqCursor += 2;\n        return value;\n#endif", 1),
    ("        mSeqCursor--;\n#ifdef __MWERKS__\n        return (*((u32*)mSeqCursor)++) & 0x00ffffff;\n#else\n        u32* value = (u32*)mSeqCursor;\n"
     "        mSeqCursor += 4;\n        return *value & 0x00ffffff;\n#endif",
     "#ifdef __MWERKS__\n        mSeqCursor--;\n        return (*((u32*)mSeqCursor)++) & 0x00ffffff;\n#else\n"
     "        const u32 value = u32(mSeqCursor[0]) << 16 | u32(mSeqCursor[1]) << 8 | mSeqCursor[2];\n        mSeqCursor += 3;\n"
     "        return value;\n#endif", 1),
])
# The audio archive's commands and arguments are big-endian words.
adapt("libs/JSystem/include/JSystem/JAudio2/JAUAudioArcInterpreter.hpp", "include/JSystem/JAudio2/JAUAudioArcInterpreter.hpp", [
    ("        u32 temp = *(u32*)mReadPtr;\n", "        const u32 temp = u32(mReadPtr[0]) << 24 | u32(mReadPtr[1]) << 16 | u32(mReadPtr[2]) << 8 | mReadPtr[3];\n", 1),
])

# GX display list writing, whose inline writers every J3D-facing header reaches.
adapt("src/RVL_SDK/gd/GDBase.c", "RVL_SDK/gd/GDBase.cpp", [
    ('#include "revolution/gd/GDBase.h"', '#include "revolution/gd/GDBase.h"\n#include <revolution/os/OSCache.h>\n#include <cstddef>\n#include <cstdint>', 1),
    ("u32 n = ((u32)__GDCurrentDL->ptr & 31);", "u32 n = ((uintptr_t)__GDCurrentDL->ptr & 31);", 1),
    # C converts void* implicitly; C++ spells it.
    ("pDL->start = pStart;", "pDL->start = static_cast< u8* >(pStart);", 1),
])

# The Wii's write-gather pipe to its GPU: the port's FIFO takes its writes (source/compat/GxFifo.cpp).
gx_vert = (root / "libs/RVL_SDK/include/revolution/gx/GXVert.h").read_text()
gx_vertex_writers = gx_vert[gx_vert.index("#define __GXCDEF1(func, ts, td)"):gx_vert.index("__GXCDEF(GXCmd, 1, u8)")]
adapt("libs/RVL_SDK/include/revolution/gx/GXVert.h", "include/revolution/gx/GXVert.h", [
    ("#ifdef __MWERKS__\nvolatile PPCWGPipe GXWGFifo : 0xCC008000;\n#else\nvolatile PPCWGPipe GXWGFifo;\n#endif",
     "#ifdef __MWERKS__\nvolatile PPCWGPipe GXWGFifo : 0xCC008000;\n#define GXWGFIFO_PUT(td, v) (GXWGFifo.td = (td)(v))\n"
     "#define GXWGFIFO_PUT2(td, x, y) (GXWGFIFO_PUT(td, x), GXWGFIFO_PUT(td, y))\n"
     "#define GXWGFIFO_PUT3(td, x, y, z) (GXWGFIFO_PUT2(td, x, y), GXWGFIFO_PUT(td, z))\n"
     "#define GXWGFIFO_PUT4(td, x, y, z, w) (GXWGFIFO_PUT3(td, x, y, z), GXWGFIFO_PUT(td, w))\n#else\n"
     "extern volatile PPCWGPipe GXWGFifo;\n"
     "// The pipe to the GPU is the port's FIFO, which takes each value big-endian as the Wii's pipe does.\n"
     "#include <smg/GxFifoWrite.h>\n#define GXWGFIFO_PUT(td, v) smgGXFifo_##td((td)(v))\n"
     "// A vertex's values of one type go in with one look for room.\n"
     "#define GXWGFIFO_BITS(td, v) SMG_GX_FIFO_BITS_##td((td)(v))\n"
     "#define GXWGFIFO_PIECE(td, v) GXWGFIFO_BITS(td, v), SMG_GX_FIFO_BYTES_##td\n"
     "#define GXWGFIFO_PUT2(td, x, y) smgGXFifoPut2(GXWGFIFO_PIECE(td, x), GXWGFIFO_PIECE(td, y))\n"
     "#define GXWGFIFO_PUT3(td, x, y, z) smgGXFifoPut3(GXWGFIFO_PIECE(td, x), GXWGFIFO_PIECE(td, y), GXWGFIFO_PIECE(td, z))\n"
     "#define GXWGFIFO_PUT4(td, x, y, z, w) smgGXFifoPut4(GXWGFIFO_PIECE(td, x), GXWGFIFO_PIECE(td, y), GXWGFIFO_PIECE(td, z), GXWGFIFO_PIECE(td, w))\n#endif", 1),
    # Inline, so a file that includes the writers without calling them emits none.
    (gx_vertex_writers,
     "#define __GXCDEF1(func, ts, td) static inline void func(const ts x) { GXWGFIFO_PUT(td, x); }\n"
     "#define __GXCDEF2(func, ts, td) static inline void func(const ts x, const ts y) { GXWGFIFO_PUT2(td, x, y); }\n"
     "#define __GXCDEF3(func, ts, td) static inline void func(const ts x, const ts y, const ts z) { GXWGFIFO_PUT3(td, x, y, z); }\n"
     "#define __GXCDEF4(func, ts, td) static inline void func(const ts x, const ts y, const ts z, const ts w) { GXWGFIFO_PUT4(td, x, y, z, w); }\n\n", 1),
])

# J3D's own index loads name its double-buffered draw matrices, which calcAnim rewrites while the render thread is still
# a frame behind: they go as immediate loads of what the index names when sent (source/compat/GxFifo.cpp).
adapt("libs/JSystem/include/JSystem/J3DGraphBase/J3DFifo.hpp", "include/JSystem/J3DGraphBase/J3DFifo.hpp", [
    ("#include <revolution/gx/GXVert.h>\n",
     "#include <revolution/gx/GXVert.h>\n\nextern \"C\" void smgGXLoadIndexedNow(unsigned command, unsigned index, unsigned target);\n\n"
     "// The same commands as pieces of a run (smgGXFifoRun).\n"
     "template <class Out> inline void J3DFifoLoadCPCmd(Out& out, u8 reg, u32 value) {\n    out.u8(0x08);\n    out.u8(reg);\n    out.u32(value);\n}\n\n"
     "template <class Out> inline void J3DFifoLoadXFCmdHdr(Out& out, u16 addr, u8 len) {\n    out.u8(0x10);\n    out.u16(len - 1);\n    out.u16(addr);\n}\n", 1),
    ("    GXWGFifo.u8 = cmd;\n    GXWGFifo.u16 = indx;\n    GXWGFifo.u16 = addr;", "    smgGXLoadIndexedNow(cmd, indx, addr);", 1),
    ("    GXCmd1u8(0x28);\n    GXCmd1u16(index);\n    GXCmd1u16(((9 - 1) << 12) | (u16)((addr * 3) + 0x400));",
     "    smgGXLoadIndexedNow(0x28, index, ((9 - 1) << 12) | (u16)((addr * 3) + 0x400));", 1),
    # A command's pieces go in with one look for room.
    ("    GXCmd1u8(0x10);\n    GXCmd1u16(len - 1);\n    GXCmd1u16(addr);", "    smgGXFifoPut3(0x10, 1, len - 1, 2, addr, 2);", 2),
    ("    GXCmd1u8(0x08);\n    GXCmd1u8(reg);\n    GXCmd1u32(value);", "    smgGXFifoPut3(0x08, 1, reg, 1, value, 4);", 1),
    ("    GXCmd1u8(0x61);\n    GXCmd1u32(regval);", "    smgGXFifoPut2(0x61, 1, regval, 4);", 1),
])
# A shape's current matrix indices (two CP loads, an XF load of the same two words) go in with one look for room.
adapt("libs/JSystem/include/JSystem/J3DGraphBase/J3DShape.hpp", "include/JSystem/J3DGraphBase/J3DShape.hpp", [
    ("    void load() const {\n        J3DFifoLoadCPCmd(0x30, mMtxIdxRegA);\n        J3DFifoLoadCPCmd(0x40, mMtxIdxRegB);\n"
     "        J3DFifoLoadXFCmdHdr(0x1018, 2);\n        GXCmd1u32(mMtxIdxRegA);\n        GXCmd1u32(mMtxIdxRegB);\n    }",
     "    void load() const {\n        smgGXFifoRun(6 + 6 + 5 + 4 + 4, [&](auto& out) {\n            J3DFifoLoadCPCmd(out, 0x30, mMtxIdxRegA);\n"
     "            J3DFifoLoadCPCmd(out, 0x40, mMtxIdxRegB);\n            J3DFifoLoadXFCmdHdr(out, 0x1018, 2);\n"
     "            out.u32(mMtxIdxRegA);\n            out.u32(mMtxIdxRegB);\n        });\n    }", 1),
])
# J3D's deformed vertices are double-buffered by calcAnim, and the render thread may still read the last drawn frame's:
# they swap only when a frame has been drawn since (smgSwapSinceDrawn, source/compat/GxFifo.cpp).
adapt("src/JSystem/J3DGraphAnimator/J3DCluster.cpp", "JSystem/J3DGraphAnimator/J3DCluster.cpp", [
    ('#include "JSystem/JMath/JMATrigonometric.hpp"\n',
     '#include "JSystem/JMath/JMATrigonometric.hpp"\n\nextern "C" int smgSwapSinceDrawn(const void* owner);\n', 1),
    ("    buffer->swapVtxPosArrayPointer();\n    buffer->swapVtxNrmArrayPointer();\n",
     "    if (smgSwapSinceDrawn(buffer)) {\n        buffer->swapVtxPosArrayPointer();\n        buffer->swapVtxNrmArrayPointer();\n    }\n", 1),
])
# The shape's display list is the GPU's, big-endian: its primitives' vertex counts and 16-bit indices are read so
# (a native load gave an index past the fur's index array).
adapt("src/Game/Util/FurShader.cpp", "Game/Util/FurShader.cpp", [
    ('#include "Game/Util/FurShader.hpp"\n',
     '#include "Game/Util/FurShader.hpp"\n\nextern "C" int smgSwapSinceDrawn(const void* owner);\n'
     'static inline u16 smgListHalf(const u8* p) { return u16(p[0] << 8 | p[1]); }\n', 1),
    ("    pModel->mVertexBuffer.swapTransformedVtxPos();\n",
     "    if (smgSwapSinceDrawn(&pModel->mVertexBuffer)) {\n        pModel->mVertexBuffer.swapTransformedVtxPos();\n    }\n", 1),
    ("        return *reinterpret_cast< u16* >(pRead + 1);", "        return smgListHalf(pRead + 1);", 1),
    ("                u16 pos = *reinterpret_cast< u16* >(pVertex + posOffset);\n"
     "                u16 nrm = *reinterpret_cast< u16* >(pVertex + nrmOffset);\n"
     "                u16 tex = *reinterpret_cast< u16* >(pVertex + texOffset);",
     "                u16 pos = smgListHalf(pVertex + posOffset);\n"
     "                u16 nrm = smgListHalf(pVertex + nrmOffset);\n"
     "                u16 tex = smgListHalf(pVertex + texOffset);", 1),
    ("                s32 count = *reinterpret_cast< u16* >(pRead + 1);", "                s32 count = smgListHalf(pRead + 1);", 1),
    ("                    u16 pos = *reinterpret_cast< u16* >((pRead + stride * i) + (posOffset + 3));",
     "                    u16 pos = smgListHalf((pRead + stride * i) + (posOffset + 3));", 1),
])
# The fur map is an IA8 texture the GPU reads by byte, alpha first: its texels are stored as the Wii's halves were.
adapt("src/Game/Util/FurDrawer.cpp", "Game/Util/FurDrawer.cpp", [
    ("        pPixels[i] = 255;", "        pPixels[i] = __builtin_bswap16(255);", 1),
    ("            pPixels[x + width * y] = (u16(255.0f * mThickness[type]) << 8) | u16(255 - mLengthMap[type]);",
     "            pPixels[x + width * y] = __builtin_bswap16((u16(255.0f * mThickness[type]) << 8) | u16(255 - mLengthMap[type]));", 1),
])
# A prohibited section ends with a yield, so a thread the Wii's scheduler held back runs. The port's scheduler holds
# none back (OSDisableScheduler only counts): a higher thread waiting for the CPU runs as the section lets it go, so
# the yield is only a kernel call, once for each material whose display list is patched.
adapt("src/Game/Util/SchedulerUtil.cpp", "Game/Util/SchedulerUtil.cpp", [
    ("    OSEnableScheduler();\n    OSYieldThread();\n", "    OSEnableScheduler();\n", 1),
])
# Material and packet display lists are double-buffered by calcAnim as well: their commands go into the FIFO when called.
adapt("src/JSystem/J3DGraphBase/J3DPacket.cpp", "JSystem/J3DGraphBase/J3DPacket.cpp", [
    ("#include <mem.h>\n", "#include <mem.h>\n#include <smg/GxPrivate.h>\n", 1),
    ("    GXCallDisplayList(mpDisplayList[0], mSize);", "    smgGXCallDisplayListNow(mpDisplayList[0], mSize);", 1),
])

# Petari's own Inline.hpp brings NO_INLINE back as an attribute; for GCC it is empty, as types.h has it.
adapt("include/Inline.hpp", "include/Inline.hpp", [
    ("#define NO_INLINE __attribute__((noinline))", "#define NO_INLINE", 1),
])
# Metrowerks on Windows opened headers whatever the case of the name in the include.
for included, present in [
    ("Game/Enemy/KariKariDirector.hpp", "Game/Enemy/KarikariDirector.hpp"),
    ("Game/Animation/Xanimecore.hpp", "Game/Animation/XanimeCore.hpp"),
    ("Game/Map/FileSelectItem.hpp", "Game/Map/FIleSelectItem.hpp"),
    ("JSystem/JAudio2/JASDSPInterface.hpp", "JSystem/JAudio2/JASDspInterface.hpp"),
    ("revolution/FS.h", "revolution/fs.h"),
    ("revolution/MEM.h", "revolution/mem.h"),
    ("revolution/NAND.h", "revolution/nand.h"),
]:
    alias = output / "include" / included
    alias.parent.mkdir(parents=True, exist_ok=True)
    alias_text = f'// Generated Petari adaptation: Metrowerks found {present} under this case.\n#include "{present}"\n'
    emit(alias, alias_text)
# Comparisons of a bool or a pointer-holding u32 with nullptr, which Metrowerks took as with 0.
adapt("src/Game/Player/Mario.cpp", "Game/Player/Mario.cpp", [
    ("if (MR::normalizeOrZero(&front) == nullptr) {", "if (!MR::normalizeOrZero(&front)) {", 1),
    ("if (MR::normalizeOrZero(&headVec) != nullptr) {", "if (MR::normalizeOrZero(&headVec)) {", 1),
])
adapt("src/Game/Player/MarioActorParts.cpp", "Game/Player/MarioActorParts.cpp", [
    ("if (_468 == nullptr) {", "if (_468 == 0) {", 1),
])
adapt("src/Game/Player/MarioActorRushMsg.cpp", "Game/Player/MarioActorRushMsg.cpp", [
    ("        if (out != nullptr) {", "        if (out) {", 1),
])
# The union's struct names went with Mario.hpp's adaptation; the array is the same storage.
adapt("src/Game/Player/MarioInit.cpp", "Game/Player/MarioInit.cpp", [
    ("    _8B0.zero();\n    _8BC.zero();", "    mRelativeWallPos[0].zero();\n    mRelativeWallPos[1].zero();", 1),
])
adapt("src/Game/Player/MarioEnforce.cpp", "Game/Player/MarioEnforce.cpp", [
    ("pPosition, &_8B0);", "pPosition, &mRelativeWallPos[0]);", 1),
    ("pPosition, &_8BC);", "pPosition, &mRelativeWallPos[1]);", 1),
])
# Cases that declare at case level, which later labels jump past; each gets its own braces.
adapt("src/Game/Player/MarioActor.cpp", "Game/Player/MarioActor.cpp", [
    ("    case 1:\n        bool didSpinPunch = true;", "    case 1: {\n        bool didSpinPunch = true;", 1),
    # Metrowerks packs bitfields from each word's top bit, GCC on ARM from its bottom: this bit is field 0x37.
    ("        if (mMario->mMovementStates_HIGH_WORD >> 8 & 1) {", "        if (mMario->mMovementStates._37) {", 1),
    ("            _946 = mConst->getTable()->mSpinIntervalTime + 0x22;\n        }\n\n        break;\n    case 2:",
     "            _946 = mConst->getTable()->mSpinIntervalTime + 0x22;\n        }\n\n        break;\n    }\n    case 2:", 1),
    ("        const MarioConstTable* pConstants = mConst->getTable();\n        _946 = pConstants->mTeresaWallThroughTime + pConstants->mSpinIntervalTime;\n        break;\n    case 5:",
     "        {\n            const MarioConstTable* pConstants = mConst->getTable();\n            _946 = pConstants->mTeresaWallThroughTime + pConstants->mSpinIntervalTime;\n        }\n        break;\n    case 5:", 1),
])
# An out-of-class member declaration kept only for matching, and a case that jumps past a declaration.
adapt("src/Game/Player/MarioWarp.cpp", "Game/Player/MarioWarp.cpp", [
    ("namespace JGeometry {\n    TVec3< f32 > TVec3< f32 >::operator*(f32) const NO_INLINE;\n}\n", "", 1),
    ("        Mario* player = getPlayer();\n        player->_42A = 0;\n        player->_430 = 0;\n        break;\n    case 1:",
     "        {\n            Mario* player = getPlayer();\n            player->_42A = 0;\n            player->_430 = 0;\n        }\n        break;\n    case 1:", 1),
])

# LiveActor without its sound setup, which needs the audio system: a caller fails to link rather than run.
adapt("src/Game/LiveActor/LiveActor.cpp", "Game/LiveActor/LiveActor.cpp", [
    ('#include "Game/AudioLib/AudAnmSoundObject.hpp"\n', "", 1),
], drop=["void LiveActor::initSound(int param1, bool is2D) {", "void LiveActor::addToSoundObjHolder() {"],
   reason="sound comes with the audio system")

# The Binder's flags are a byte of bitfields, which these set by its mask: 0x20 is _2 for Metrowerks (first field in
# the top bit) and _5 for GCC (first in the bottom), so the field is named instead; 0x80 is _0.
adapt("src/Game/Util/LiveActorUtil.cpp", "Game/Util/LiveActorUtil.cpp", [
    ("        *(u8*)&pActor->mBinder->_1EC |= 0x20;", "        pActor->mBinder->_1EC._2 = true;", 1),
    ("        *(u8*)&pActor->mBinder->_1EC &= ~0x20;", "        pActor->mBinder->_1EC._2 = false;", 1),
    ("        s8* pFlags = (s8*)&pActor->mBinder->_1EC;\n        s32 flags = *pFlags;\n        flags &= 0xFFFFFF7F;\n        *pFlags = flags;",
     "        pActor->mBinder->_1EC._0 = false;", 1),
])

# Actors: what LiveActor, its sensors and its Binder call from the utility files, which reach the whole game.
extract("src/Game/Util/LiveActorUtil.cpp", "Game/Util/LiveActorUtilCore.cpp",
        '#include "Game/LiveActor/ActorLightCtrl.hpp"\n#include "Game/LiveActor/LiveActor.hpp"\n'
        '#include "Game/Map/CollisionParts.hpp"\n#include "Game/Util/GravityUtil.hpp"\n#include "Game/Util/LiveActorUtil.hpp"\n'
        '#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/ModelUtil.hpp"\n#include "Game/LiveActor/ClippingActorHolder.hpp"\n'
        '#include "Game/LiveActor/ClippingDirector.hpp"\n#include "Game/LiveActor/Binder.hpp"\n',
        [("", "void calcCollisionMtx(TPos3f* pMtx, const LiveActor* pActor) NO_INLINE {"),
         ("MR", "void calcGravity(LiveActor* pActor) {"), ("MR", "bool isCalcGravity(const LiveActor* pActor) {"),
         ("MR", "void onBind(LiveActor* pActor) {"), ("MR", "bool isDead(const LiveActor* pActor) {"),
         ("MR", "void setBaseTRMtx(LiveActor* pActor, MtxPtr pMtx) {"),
         ("MR", "void setBaseTRMtx(LiveActor* pActor, const TPos3f& rMtx) {"),
         ("MR", "void setCollisionMtx(LiveActor* pActor) {"), ("MR", "void setCollisionMtx(LiveActor* pActor, CollisionParts* pParts) {"),
         ("MR", "CollisionParts* getCollisionParts(const LiveActor* pActor) {"),
         ("MR", "void validateCollisionParts(LiveActor* pActor) {"), ("MR", "void validateCollisionParts(CollisionParts* pParts) {"),
         ("MR", "void invalidateCollisionParts(LiveActor* pActor) {"), ("MR", "void invalidateCollisionParts(CollisionParts* pParts) {"),
         ("MR", "void resetAllCollisionMtx(LiveActor* pActor) {"), ("MR", "bool isClipped(const LiveActor* pActor) {"),
         ("MR", "bool isNoEntryDrawBuffer(const LiveActor* pActor) {"), ("MR", "bool isHiddenModel(const LiveActor* pActor) {"),
         ("MR", "bool isInvalidClipping(const LiveActor* pActor) {"),
         ("MR", "bool isOnGround(const LiveActor* pActor) {"), ("MR", "bool isBindedGround(const LiveActor* pActor) {"),
         ("MR", "void onCalcGravity(LiveActor* pActor) {"), ("MR", "void invalidateClipping(LiveActor* pActor) {"),
         ("MR", "void calcAnimDirect(LiveActor* pActor) {")])
extract("src/Game/Util/ActorSensorUtil.cpp", "Game/Util/ActorSensorUtilCore.cpp",
        '#include "Game/LiveActor/HitSensor.hpp"\n#include "Game/LiveActor/HitSensorKeeper.hpp"\n'
        '#include "Game/LiveActor/LiveActor.hpp"\n#include "Game/Util/ActorSensorUtil.hpp"\n',
        [("MR", "bool tryUpdateHitSensorsAll(LiveActor* pActor) {"), ("MR", "void updateHitSensorsAll(LiveActor* pActor) {"),
         ("MR", "void clearHitSensors(LiveActor* pActor) {"), ("MR", "void setHitSensorApart(HitSensor* pSender, HitSensor* pReceiver) {"),
         ("MR", "HitSensor* getTaken(const LiveActor* pActor) {"), ("MR", "HitSensor* getTaking(const LiveActor* pActor) {"),
         ("MR", "LiveActor* getSensorHost(const HitSensor* pSensor) {"),
         ("MR", "bool isSensorPlayer(const HitSensor* pSensor) {"), ("MR", "bool isSensorRide(const HitSensor* pSensor) {"),
         ("MR", "bool isSensorMapObj(const HitSensor* pSensor) {"), ("MR", "bool isSensorAutoRush(const HitSensor* pSensor) {"),
         ("MR", "bool isSensorRush(const HitSensor* pSensor) {"),
         ("MR", "HitSensor* addHitSensor(LiveActor* pActor, const char* pName, u32 type, u16 groupSize, f32 radius, const TVec3f& rOffset) {"),
         ("MR", "HitSensor* addHitSensorMapObj(LiveActor* pActor, const char* pName, u16 groupSize, f32 radius, const TVec3f& rOffset) {"),
         ("MR", "HitSensor* addHitSensorBinder(LiveActor* pActor, const char* pName, u16 groupSize, f32 radius, const TVec3f& rOffset) {")])
extract("src/Game/Util/ActorMovementUtil.cpp", "Game/Util/ActorMovementUtilCore.cpp",
        '#include "Game/LiveActor/Binder.hpp"\n#include "Game/LiveActor/LiveActor.hpp"\n#include "Game/Util/ActorMovementUtil.hpp"\n'
        '#include "Game/Util/ActorSensorUtil.hpp"\n#include "Game/Util/LiveActorUtil.hpp"\n#include "Game/Util/MtxUtil.hpp"\n'
        '#include "Game/LiveActor/ShadowController.hpp"\n',
        [("MR", "void resetPosition(LiveActor* pActor) {"), ("MR", "void makeMtxTR(MtxPtr pMtx, const LiveActor* pActor) {"),
         ("MR", "void makeMtxTRS(MtxPtr pMtx, const LiveActor* pActor) {"),
         ("MR", "void makeMtxTransRotateY(MtxPtr pMtx, const LiveActor* pActor) {")])
extract("src/Game/LiveActor/ShadowController.cpp", "Game/LiveActor/ShadowRequests.cpp",
        '#include "Game/LiveActor/LiveActor.hpp"\n#include "Game/LiveActor/ShadowController.hpp"\n#include "Game/Util/SceneUtil.hpp"\n',
        [("MR", "void requestCalcActorShadowAppear(LiveActor* pActor) {"), ("MR", "void requestCalcActorShadow(LiveActor* pActor) {")])
extract("src/Game/Util/ObjUtil.cpp", "Game/Util/ObjUtilScene.cpp",
        '#include "Game/LiveActor/LiveActor.hpp"\n#include "Game/NameObj/NameObjExecuteHolder.hpp"\n#include "Game/Util/ObjUtil.hpp"\n',
        ["void MR::connectToScene(LiveActor* pActor, int movementType, int calcAnimType, int drawBufferType, int drawType) {",
         "void MR::connectToScene(NameObj* pObj, int movementType, int calcAnimType, int drawBufferType, int drawType) {"])
extract("src/Game/Util/MapUtil.cpp", "Game/Util/MapUtilCore.cpp",
        '#include "Game/Map/CollisionCategorizedKeeper.hpp"\n#include "Game/Map/CollisionDirector.hpp"\n'
        '#include "Game/Util/MapUtil.hpp"\n#include "Game/Util/MathUtil.hpp"\n',
        [("", "u32 getStrikeInfoNumCategory(s32 category) {"),
         ("MR", "bool isWallPolygon(f32 param1) {"), ("MR", "bool isFloorPolygon(f32 param1) {"),
         ("MR", "bool isFloorPolygon(const TVec3f& rParam1, const TVec3f& rParam2) {"),
         ("MR", "bool isWallPolygon(const TVec3f& rParam1, const TVec3f& rParam2) {"),
         ("Collision", "s32 checkStrikePointToMap(const TVec3f& rPoint, HitInfo* pHit) {"),
         ("Collision", "s32 checkStrikeBallToMap(const TVec3f& rCenter, f32 radius, const CollisionPartsFilterBase* pPartsFilter,"),
         ("Collision", "s32 checkStrikeBallToMapWithMovingReaction(const TVec3f& rCenter, f32 radius, const CollisionPartsFilterBase* pPartsFilter,"),
         ("Collision", "s32 checkStrikeLineToMap(const TVec3f& rStart, const TVec3f& rDirection, s32 count, const CollisionPartsFilterBase* pPartsFilter,"),
         ("Collision", "const HitInfo* getStrikeInfoMap(u32 index) {"), ("Collision", "u32 getStrikeInfoNumMap() {")])

# J3D's paired-single routines, kept for Metrowerks, with plain C++ for other compilers.
adapt("src/JSystem/J3DGraphBase/J3DTransform.cpp", "JSystem/J3DGraphBase/J3DTransform.cpp", [
    ("asm void J3DPSCalcInverseTranspose(", "void J3DPSCalcInverseTranspose(", 1),
    # The rows are cross products of the source rows, over the determinant; a zero determinant leaves dst alone.
    ("\tpsq_st   f8, 32(r4), 1, 0\n\tblr\n#endif  // clang-format on\n}", """\tpsq_st   f8, 32(r4), 1, 0
\tblr
#else
    const f32 (*a)[4] = src;
    const f32 rows[3][3]{{a[1][1] * a[2][2] - a[1][2] * a[2][1], a[1][2] * a[2][0] - a[1][0] * a[2][2], a[1][0] * a[2][1] - a[1][1] * a[2][0]},
                         {a[2][1] * a[0][2] - a[2][2] * a[0][1], a[2][2] * a[0][0] - a[2][0] * a[0][2], a[2][0] * a[0][1] - a[2][1] * a[0][0]},
                         {a[0][1] * a[1][2] - a[0][2] * a[1][1], a[0][2] * a[1][0] - a[0][0] * a[1][2], a[0][0] * a[1][1] - a[0][1] * a[1][0]}};
    const f32 det = a[0][0] * rows[0][0] + a[1][0] * rows[1][0] + a[2][0] * rows[2][0];
    if (det == 0.0f) {
        return;
    }
    const f32 inverse = 1.0f / det;
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
            dst[row][column] = rows[row][column] * inverse;
        }
    }
#endif  // clang-format on
}""", 1),
    ("asm void J3DScaleNrmMtx(", "void J3DScaleNrmMtx(", 1),
    ("\tstfs   f4, 40(mtx)\n\tblr\n#endif  // clang-format on\n}", """\tstfs   f4, 40(mtx)
\tblr
#else
    for (int row = 0; row < 3; row++) {
        mtx[row][0] *= scl.x;
        mtx[row][1] *= scl.y;
        mtx[row][2] *= scl.z;
    }
#endif  // clang-format on
}""", 1),
    ("asm void J3DScaleNrmMtx33(", "void J3DScaleNrmMtx33(", 1),
    ("\tstfs     f5, 0x20(mtx)\n\tblr\n#endif  // clang-format on\n}", """\tstfs     f5, 0x20(mtx)
\tblr
#else
    for (int row = 0; row < 3; row++) {
        mtx[row][0] *= scale.x;
        mtx[row][1] *= scale.y;
        mtx[row][2] *= scale.z;
    }
#endif  // clang-format on
}""", 1),
    # A 3x4 times a 4x4: all four of mtx1's columns weigh mtx2's rows.
    ("asm void J3DMtxProjConcat(", "void J3DMtxProjConcat(", 1),
    ("\tpsq_st   f0, 40(dst), 0, 0\n\tblr\n#endif  // clang-format on\n}", """\tpsq_st   f0, 40(dst), 0, 0
\tblr
#else
    f32 out[3][4];
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
            out[row][column] = mtx1[row][0] * mtx2[0][column] + mtx1[row][1] * mtx2[1][column] + mtx1[row][2] * mtx2[2][column] +
                               mtx1[row][3] * mtx2[3][column];
        }
    }
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
            dst[row][column] = out[row][column];
        }
    }
#endif  // clang-format on
}""", 1),
    ("#undef UNIT_R\n}\n#endif  // clang-format on", """#undef UNIT_R
}
#else
// mAB[i] = mA mB[i] for count matrices, as PSMTXConcat does each.
void J3DPSMtxArrayConcat(Mtx mA, Mtx mB, Mtx mAB, u32 count) {
    for (u32 i = 0; i < count; i++) {
        PSMTXConcat(mA, mB, mAB);
        mB += 3;
        mAB += 3;
    }
}
#endif  // clang-format on""", 1),
])
# J3DModelX's draw flags are a bitfield read back as a word to pick extra display lists, list i when bit i is set.
# Metrowerks packs a struct's first bitfield into the word's top bit, GCC into its bottom, so flag _1E (the shadow
# capture's list 1) sits at bit 30 here: list i is the flag GCC keeps at bit 31 - i. Mario's dynamic lists (his light
# and alpha settings, rewritten by calcAnim in a ring of two) go into the FIFO when called, as J3D's own do.
adapt("src/Game/Player/J3DModelX.cpp", "Game/Player/J3DModelX.cpp", [
    ("if (*reinterpret_cast< u32* >(&mFlags) & (1 << i)) {", "if (*reinterpret_cast< u32* >(&mFlags) & (1u << (31 - i))) {", 1),
    ('#include <revolution/gd.h>\n', '#include <revolution/gd.h>\n#include <smg/GxPrivate.h>\n', 1),
    ("        GXCallDisplayList(_1B4, _1BC);", "        smgGXCallDisplayListNow(_1B4, _1BC);", 1),
    ("        GXCallDisplayList(_1C4[index], _1CC[index]);", "        smgGXCallDisplayListNow(_1C4[index], _1CC[index]);", 1),
])
# Each weighted envelope is its joints' animated matrices times their inverse binds, weighted; its scale flag
# holds only if every joint in the mix is unscaled.
adapt("src/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp", "JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp", [
    ("void J3DMtxBuffer::calcWeightEnvelopeMtx() {\n    __REGISTER MtxPtr weightAnmMtx;",
     "#ifdef __MWERKS__\nvoid J3DMtxBuffer::calcWeightEnvelopeMtx() {\n    __REGISTER MtxPtr weightAnmMtx;", 1),
    ("            psq_st var_f30, 40(weightAnmMtx), 0, 0\n            ps_merge00 var_f30, var_f24, var_f24\n        }\n    }\n}\n",
     """            psq_st var_f30, 40(weightAnmMtx), 0, 0
            ps_merge00 var_f30, var_f24, var_f24
        }
    }
}
#else
void J3DMtxBuffer::calcWeightEnvelopeMtx() {
    const u16 max = mJointTree->getWEvlpMtxNum();
    const u16* indices = mJointTree->getWEvlpMixMtxIndex();
    const f32* weights = mJointTree->getWEvlpMixWeight();
    for (u16 i = 0; i < max; i++) {
        u8* pScale = &mpEvlpScaleFlagArr[i];
        *pScale = 1;
        f32 sum[3][4] = {};
        const u8 mixNum = mJointTree->getWEvlpMixMtxNum(i);
        for (u8 j = 0; j < mixNum; j++) {
            const u16 idx = *indices++;
            const f32 weight = *weights++;
            Mtx product;
            PSMTXConcat(mpAnmMtx[idx], mJointTree->getInvJointMtx(idx), product);
            for (int row = 0; row < 3; row++) {
                for (int column = 0; column < 4; column++) {
                    sum[row][column] += product[row][column] * weight;
                }
            }
            *pScale &= mpScaleFlagArr[idx];
        }
        MtxPtr weightAnmMtx = mpWeightEvlpMtx[i];
        for (int row = 0; row < 3; row++) {
            for (int column = 0; column < 4; column++) {
                weightAnmMtx[row][column] = sum[row][column];
            }
        }
    }
}
#endif
""", 1),
])
# Vertex color index tables: a base pointer plus an index held in the pointer field, through intptr_t so a
# 64-bit host keeps the whole address.
adapt("src/JSystem/J3DGraphLoader/J3DAnmLoader.cpp", "JSystem/J3DGraphLoader/J3DAnmLoader.cpp", [
    ("(void*)((s32)indexPtr0 + (s32)dst->mAnmVtxColorIndexData[0][i].mpData * 2)",
     "(void*)((intptr_t)indexPtr0 + (intptr_t)dst->mAnmVtxColorIndexData[0][i].mpData * 2)", 2),
    ("(void*)((s32)indexPtr1 + (s32)dst->mAnmVtxColorIndexData[1][i].mpData * 2)",
     "(void*)((intptr_t)indexPtr1 + (intptr_t)dst->mAnmVtxColorIndexData[1][i].mpData * 2)", 2),
    # Animations turn native as the loader first meets them (smg::nativeJ3DAnimation).
    ('#include "JSystem/JSupport/JSupport.hpp"\n', '#include "JSystem/JSupport/JSupport.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("    if (!i_data) {\n        return NULL;\n    }\n", "    if (!i_data) {\n        return NULL;\n    }\n    smgNativeJ3DAnimation(const_cast< void* >(i_data));\n", 1),
])
# The loader's first line reads the current JKRHeap's free size and drops it; the port allocates from the free store.
adapt("src/JSystem/J3DGraphLoader/J3DModelLoader.cpp", "JSystem/J3DGraphLoader/J3DModelLoader.cpp", [
    ("J3DModelData* J3DModelLoader::load(void const* i_data, u32 i_flags) {\n    JKRGetCurrentHeap()->getTotalFreeSize();\n",
     "J3DModelData* J3DModelLoader::load(void const* i_data, u32 i_flags) {\n", 1),
    # Models turn native as JSystem's loaders first meet them (compat/smg/NativeResources.hpp, smg::nativeJ3DModel).
    ('#include "JSystem/JUtility/JUTNameTab.hpp"\n', '#include "JSystem/JUtility/JUTNameTab.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("    if (i_data == NULL) {\n        return NULL;\n    }\n", "    if (i_data == NULL) {\n        return NULL;\n    }\n    smgNativeJ3DModel(const_cast< void* >(i_data));\n", 1),
    ("    if (data == NULL) {\n        return NULL;\n    }\n", "    if (data == NULL) {\n        return NULL;\n    }\n    smgNativeJ3DModel(const_cast< void* >(data));\n", 2),
])
# GDSetArrayRaw takes the array's raw address as a u32; Metrowerks converted the null to it.
adapt("src/JSystem/J3DGraphBase/J3DShape.cpp", "JSystem/J3DGraphBase/J3DShape.cpp", [
    ("GDSetArrayRaw((GXAttr)(i + GX_VA_POS), nullptr, stride[i]);", "GDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);", 1),
    # Clearing the top bit was the Wii's cached-to-physical step; the port's addresses need the SDK's own macro,
    # which the command processor undoes.
    ("#include <revolution/gd.h>\n", "#include <revolution/gd.h>\n#include <revolution/os.h>\n", 1),
    ("J3DLoadCPCmd(0xA0 + idx, ((uintptr_t)data & 0x7FFFFFFF));", "J3DLoadCPCmd(0xA0 + idx, OSCachedToPhysical(data));", 1),
    # The three array bases are one run of CP loads, in one look for room.
    ("static void J3DLoadArrayBasePtr(GXAttr attr, void* data) {", "template <class Out> static void J3DLoadArrayBasePtr(Out& out, GXAttr attr, void* data) {", 1),
    ("    J3DLoadCPCmd(0xA0 + idx, OSCachedToPhysical(data));", "    J3DFifoLoadCPCmd(out, 0xA0 + idx, OSCachedToPhysical(data));", 1),
    ("void J3DShape::loadVtxArray() const {\n    J3DLoadArrayBasePtr(GX_VA_POS, j3dSys.getVtxPos());\n\n    if (!mHasNBT) {\n"
     "        J3DLoadArrayBasePtr(GX_VA_NRM, j3dSys.getVtxNrm());\n    }\n\n    J3DLoadArrayBasePtr(GX_VA_CLR0, j3dSys.getVtxCol());\n}",
     "void J3DShape::loadVtxArray() const {\n    smgGXFifoRun(mHasNBT ? 12 : 18, [&](auto& out) {\n"
     "        J3DLoadArrayBasePtr(out, GX_VA_POS, j3dSys.getVtxPos());\n        if (!mHasNBT) {\n"
     "            J3DLoadArrayBasePtr(out, GX_VA_NRM, j3dSys.getVtxNrm());\n        }\n"
     "        J3DLoadArrayBasePtr(out, GX_VA_CLR0, j3dSys.getVtxCol());\n    });\n}", 1),
])

# Class operator new and delete take size_t, which is unsigned int on ARM where u32 is unsigned long.
adapt("libs/JSystem/include/JSystem/JAudio2/JASHeapCtrl.hpp", "include/JSystem/JAudio2/JASHeapCtrl.hpp", [
    ('#include "JSystem/JAudio2/JASCriticalSection.hpp"', '#include "JSystem/JAudio2/JASCriticalSection.hpp"\n#include <cstddef>', 1),
    ("static void* operator new(u32 size) {", "static void* operator new(std::size_t size) {", 2),
    ("static void operator delete(void* addr, u32 size) {", "static void operator delete(void* addr, std::size_t size) {", 2),
])

# GCC refuses members with constructors in an anonymous struct; the array alone is the same storage.
adapt("include/Game/Player/Mario.hpp", "include/Game/Player/Mario.hpp", [
    ("""    union {
        struct {
            /* 0x8B0 */ TVec3f _8B0;
            /* 0x8BC */ TVec3f _8BC;
        };
        /* 0x8B0 */ TVec3f mRelativeWallPos[2];
    };""", """    /* 0x8B0 */ TVec3f mRelativeWallPos[2]; // _8B0 and _8BC in the original union""", 1),
])

adapt("libs/RVL_SDK/include/revolution/os/OSTime.h", "include/revolution/os/OSTime.h", [
    ("vu32 OS_BUS_CLOCK_SPEED;", "extern vu32 OS_BUS_CLOCK_SPEED;", 1),
])

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
    emit(destination, text)

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
emit(destination, text)

# Nerves live in anonymous namespaces as often as named ones, where GCC refuses a weak definition; an inline one
# is still a single instance wherever the name is shared, as Metrowerks' weak one was.
adapt("include/Game/LiveActor/Nerve.hpp", "include/Game/LiveActor/Nerve.hpp", [
    ("    name name::sInstance ATTRIBUTE_WEAK;", "    inline name name::sInstance;", 2),
])
# ShadowDrawer declares neither: the volume drawer introduces both for its shapes.
adapt("include/Game/LiveActor/ShadowVolumeDrawer.hpp", "include/Game/LiveActor/ShadowVolumeDrawer.hpp", [
    ("    virtual void loadModelDrawMtx() const override;\n    virtual void drawShape() const override;",
     "    virtual void loadModelDrawMtx() const;\n    virtual void drawShape() const;", 1),
])
# The game asks for area managers by name many times a frame, each a strstr through every manager; a name's index is
# fixed by cCreateTable, so the game's own thread remembers it by the name's address and text. A miss still matches
# names as a prefix compare: strstr looked for the name anywhere before checking it was at the start (1.4% of Snow
# Capsule's game thread went there).
adapt("src/Game/AreaObj/AreaObjContainer.cpp", "Game/AreaObj/AreaObjContainer.cpp", [
    ("            const char* pMatch = strstr(pName, pManager->mName);\n            return pMatch != nullptr && pMatch == pName;",
     "            return strncmp(pName, pManager->mName, strlen(pManager->mName)) == 0;", 1),
    ("AreaObjMgr* AreaObjContainer::getManager(const char* pName) const {\n"
     "    return *std::find_if(mManagerArray, mManagerArray + mNumManagers, std::binder2nd< IsManagerName, const char* >(IsManagerName(), pName));\n}",
     "extern \"C\" int smgOnGameThread();  // source/compat/GxFifo.cpp\n\n"
     "namespace {\n    struct ManagerMemo {\n        const char* mName;\n        char mText[32];\n        s32 mIndex;\n    };\n"
     "    ManagerMemo sManagerMemo[64];\n}  // namespace\n\n"
     "AreaObjMgr* AreaObjContainer::getManager(const char* pName) const {\n"
     "    const bool memo = smgOnGameThread();\n"
     "    ManagerMemo& entry = sManagerMemo[(reinterpret_cast< u32 >(pName) >> 2) % 64];\n"
     "    if (memo && entry.mName == pName && strcmp(entry.mText, pName) == 0) {\n        return mManagerArray[entry.mIndex];\n    }\n"
     "    AreaObjMgr* const* found = std::find_if(mManagerArray, mManagerArray + mNumManagers, std::binder2nd< IsManagerName, const char* >(IsManagerName(), pName));\n"
     "    const s32 index = found - mManagerArray;\n"
     "    if (memo && index < static_cast< s32 >(mNumManagers) && strlen(pName) < sizeof(entry.mText)) {\n"
     "        entry.mName = pName;\n        strcpy(entry.mText, pName);\n        entry.mIndex = index;\n    }\n"
     "    return *found;\n}", 1),
])
for relative in ["Game/Demo/DemoExecutor.cpp",
                 "Game/Effect/MultiEmitter.cpp", "Game/LiveActor/EffectKeeper.cpp",
                 "Game/MapObj/ElectricBall.cpp", "Game/MapObj/GeneralMapParts.cpp", "Game/Scene/MultiSceneEffectKeeper.cpp"]:
    adapt("src/" + relative, relative, [])
# Pointers and references to what a block or a statement made, used after it ended: Metrowerks left those bytes as they
# were, GCC reuses them or (for a returned reference) returns null (tools/ub-scan.sh names them). The wall normals'
# getters return a zero vector that lasts (checkGround asks for the back wall's while only _8 is set).
adapt("src/Game/Player/MarioCollision.cpp", "Game/Player/MarioCollision.cpp", [
    ("    return TVec3f(gZeroVec);\n", "    return smgWallNormZero;\n", 3),
    ("const TVec3f& Mario::getSideWallNorm() const {", "static const TVec3f smgWallNormZero(0.0f, 0.0f, 0.0f);\n\nconst TVec3f& Mario::getSideWallNorm() const {", 1),
])
adapt("src/Game/LiveActor/LiveActorGroupArray.cpp", "Game/LiveActor/LiveActorGroupArray.cpp", [
    ("    if (pName == nullptr) {\n        char defaultName[32];\n", "    char defaultName[32];\n    if (pName == nullptr) {\n", 1),
])
adapt("src/Game/Ride/Tamakoro.cpp", "Game/Ride/Tamakoro.cpp", [
    ("        // TVec3f tmpVec;\n", "        TVec3f tmpVec;\n", 1),
    ("            TVec3f tmpVec(-mGravity);\n", "            tmpVec = -mGravity;\n", 1),
])
# MSL's headers declared the narrow string functions along with the rest; the standard <cstring> does.
# The placeholder holds 8 characters (main.dol's too), so ending it at 9 wrote past it; the text shown is the same.
adapt("src/Game/Screen/LayoutCoreUtil.cpp", "Game/Screen/LayoutCoreUtil.cpp", [
    ("            s32 end = maxNumChars <= 9 ? maxNumChars : 9;", "            s32 end = maxNumChars <= 8 ? maxNumChars : 8;", 1),
])
adapt("src/Game/Util/StringUtil.cpp", "Game/Util/StringUtil.cpp", [
    ("#include <cstdio>\n#include <wstring.h>", "#include <cstdio>\n#include <cstring>\n#include <wstring.h>", 1),
    # A message's sound names are native UTF-16 here (smg::nativeBmg): a character's high byte is no longer its first.
    ("            const char* p = reinterpret_cast< const char* >(pSrc);\n\n            if (p[0] != '\\0') {\n                break;\n            }\n\n"
     "            if (p[1] == '\\0') {\n                break;\n            }\n\n            *pDst = p[1];\n",
     "            if ((*pSrc & 0xFF00) != 0) {\n                break;\n            }\n\n            if ((*pSrc & 0xFF) == 0) {\n                break;\n            }\n\n"
     "            *pDst = static_cast< char >(*pSrc);\n", 1),
])

TALK_STATE_NULLS = [
        ('    _14 = nullptr;\n', '    _14 = 0;\n', 1),
        ('    _10 = nullptr;\n', '    _10 = 0;\n', 1),
        ('    if (_1C != nullptr) {\n', '    if (_1C != 0) {\n', 1),
    ]
# Metrowerks took nullptr wherever 0 or false fit: a count, a flag, a u32 argument, a bool compared.
for relative, replacements in [
    ("Game/AudioLib/AudParams.cpp", [
        ('    const int aramAddr = nullptr;\n', '    const int aramAddr = 0;\n', 1),
    ]),
    ("Game/Boss/BossKameckStateBattle.cpp", [
        ('    if (mBattlePattarn->_10 == nullptr) {\n', '    if (mBattlePattarn->_10 == false) {\n', 1),
    ]),
    ("Game/Demo/DemoCameraFunction.cpp", [
        ('            return nullptr;\n', '            return false;\n', 1),
    ]),
    ("Game/Demo/DemoDirector.cpp", [
        ('    startDemo(DemoStartRequestUtil::getDemoStarter(info), info.mDemoName, info.mFrameType == nullptr, info._24);\n', '    startDemo(DemoStartRequestUtil::getDemoStarter(info), info.mDemoName, info.mFrameType == 0, info._24);\n', 1),
    ]),
    ("Game/Enemy/BallBeamer.cpp", [
        ('    initModelManagerWithAnm("BallBeamer", nullptr, nullptr);\n', '    initModelManagerWithAnm("BallBeamer", nullptr, false);\n', 1),
        ('    initEffectKeeper(3, nullptr, nullptr);\n', '    initEffectKeeper(3, nullptr, false);\n', 1),
        ('    initSound(2, nullptr);\n', '    initSound(2, false);\n', 1),
    ]),
    ("Game/Enemy/BegomanBase.cpp", [
        ('    if (MR::getWaterAreaObj(&info, mPosition) != nullptr) {\n', '    if (MR::getWaterAreaObj(&info, mPosition) != false) {\n', 1),
    ]),
    ("Game/Enemy/HammerHeadPackun.cpp", [
        ('    return jmap->mData ? jmap->mData->mNumEntries : nullptr;\n', '    return jmap->mData ? jmap->mData->mNumEntries : 0;\n', 1),
    ]),
    ("Game/Enemy/Mogucchi.cpp", [
        ('    MR::calcGravityVector(this, MR::getRailPos(this), &mRailGravity, nullptr, nullptr);\n', '    MR::calcGravityVector(this, MR::getRailPos(this), &mRailGravity, nullptr, 0);\n', 1),
    ]),
    ("Game/Enemy/OtaRock.cpp", [
        ('      mThrowCocoNutCounter(nullptr), mWaitFrame(0), _A4(), _D4(0.0f, 0.0f, 1.0f), mAnimScaleController(nullptr) {\n', '      mThrowCocoNutCounter(0), mWaitFrame(0), _A4(), _D4(0.0f, 0.0f, 1.0f), mAnimScaleController(nullptr) {\n', 1),
    ]),
    ("Game/MapObj/CrystalCage.cpp", [
        ('        MR::setBinderOffsetVec(this, &_110, nullptr);\n', '        MR::setBinderOffsetVec(this, &_110, false);\n', 1),
    ]),
    ("Game/MapObj/LavaFloater.cpp", [
        ('    initEffectKeeper(nullptr, nullptr, false);\n', '    initEffectKeeper(0, nullptr, false);\n', 1),
    ]),
    ("Game/MapObj/MarioLauncherAttractor.cpp", [
        ('    initModelManagerWithAnm("MarioLauncherAttractor", nullptr, nullptr);\n', '    initModelManagerWithAnm("MarioLauncherAttractor", nullptr, false);\n', 1),
    ]),
    ("Game/Map/WaterPlant.cpp", [
        ('    mPlantATex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantA.bti"), nullptr);\n', '    mPlantATex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantA.bti"), 0);\n', 1),
        ('    mPlantBTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantB.bti"), nullptr);\n', '    mPlantBTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantB.bti"), 0);\n', 1),
        ('    mPlantCTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantC.bti"), nullptr);\n', '    mPlantCTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantC.bti"), 0);\n', 1),
        ('    mPlantDTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantD.bti"), nullptr);\n', '    mPlantDTex = new JUTTexture(MR::loadTexFromArc("WaterPlant.arc", "WaterPlantD.bti"), 0);\n', 1),
    ]),
    ("Game/NPC/Syati.cpp", [
        ('    initEffectKeeper(1, nullptr, nullptr);\n', '    initEffectKeeper(1, nullptr, false);\n', 1),
    ]),
    ("Game/NPC/TalkMessageCtrl.cpp", [
        ('        if (mEventFunc->operator()(nodeEvent->mUnknown) == nullptr) {\n', '        if (mEventFunc->operator()(nodeEvent->mUnknown) == false) {\n', 1),
        ('        if (mAnimeFunc->operator()(nodeEvent->mUnknown) == nullptr) {\n', '        if (mAnimeFunc->operator()(nodeEvent->mUnknown) == false) {\n', 1),
        ('        if (mKillFunc->operator()(nodeEvent->mUnknown) == nullptr) {\n', '        if (mKillFunc->operator()(nodeEvent->mUnknown) == false) {\n', 1),
    ]),
    ("Game/NWC24/NWC24Messenger.cpp", [
        ('        if (selectTask() == nullptr) {\n', '        if (selectTask() == false) {\n', 1),
        ('            if (selectTask() != nullptr) {\n', '            if (selectTask() != false) {\n', 1),
    ]),
    ("Game/Screen/GalaxyMapSelectButton.cpp", [
        ('    return mPaneCtrl->mIsSelected != nullptr && mPaneCtrl->isDecidedWait();\n', '    return mPaneCtrl->mIsSelected != false && mPaneCtrl->isDecidedWait();\n', 1),
    ]),
    ("Game/Screen/OdhConverter.cpp", [
        ('    u32 getOdhImageSize() {\n        if (!isExistSceneObj(SceneObj_OdhConverter)) {\n            return nullptr;\n', '    u32 getOdhImageSize() {\n        if (!isExistSceneObj(SceneObj_OdhConverter)) {\n            return 0;\n', 1),
    ]),
    ("Game/Screen/PaneEffectKeeper.cpp", [
        ('    if (mHost->getLayoutManager()->isExistPaneMtxRef(pName1) == nullptr) {\n', '    if (mHost->getLayoutManager()->isExistPaneMtxRef(pName1) == false) {\n', 1),
    ]),
    ("Game/Screen/SubMeterLayout.cpp", [
        ('    MR::startAnim(this, "Appear", nullptr);\n', '    MR::startAnim(this, "Appear", 0);\n', 1),
    ]),
    ("Game/System/FileLoader.cpp", [
        ('    mRequestedFileCount = nullptr;\n', '    mRequestedFileCount = 0;\n', 1),
    ]),
    ("Game/System/GameSystemStationedArchiveLoader.cpp", [
        ('        napa->alloc(0x10000, nullptr);\n', '        napa->alloc(0x10000, 0);\n', 1),
        ('        gddr->alloc(0x10000, nullptr);\n', '        gddr->alloc(0x10000, 0);\n', 1),
    ]),
    ("Game/System/StorySequenceExecutor.cpp", [
        ('    case 12:\n        if (tryNextDemoInfo() != nullptr) {\n', '    case 12:\n        if (tryNextDemoInfo() != false) {\n', 1),
        ('    case 13:\n        if (tryNextDemoInfo() != nullptr) {\n', '    case 13:\n        if (tryNextDemoInfo() != false) {\n', 1),
    ]),
    ("Game/Util/CameraUtil.cpp", [
        ('        GXSetProjection(::getCameraContext()->mProjection, (GXProjectionType) nullptr);\n', '        GXSetProjection(::getCameraContext()->mProjection, (GXProjectionType) 0);\n', 1),
    ]),
]:
    adapt("src/" + relative, relative, replacements)

# Headers that found GX, matrix and DVD types through a Wii include chain the port's lighter headers cut short.
for relative, anchor, include in [
    ("include/Game/Screen/BloomEffect.hpp", '#include "Game/Screen/ImageEffectBase.hpp"', "<revolution/mtx.h>"),
    ("include/Game/Map/OceanSphere.hpp", '#include "Game/LiveActor/LiveActor.hpp"', "<revolution/gx/GXStruct.h>"),
    ("include/Game/Screen/StarPointerCommandStream.hpp", '#include "Game/Screen/LayoutActor.hpp"', "<revolution/gx/GXStruct.h>"),
    ("include/Game/Screen/ScreenAlphaCapture.hpp", '#include "Game/NameObj/NameObj.hpp"', "<revolution/gx/GXEnum.h>"),
    ("include/Game/Screen/GalaxyMapController.hpp", '#include "Game/Screen/LayoutActor.hpp"', "<revolution/gx/GXStruct.h>"),
    ("include/Game/System/GameSystemResetAndPowerProcess.hpp", '#include "Game/Screen/LayoutActor.hpp"', "<revolution/dvd.h>"),
    ("libs/JSystem/include/JSystem/JParticle/JPAEmitter.hpp", "#include <revolution/types.h>", "<revolution/gx/GXStruct.h>"),
]:
    adapt(relative, relative[relative.index("include/"):],
          [(anchor, f"{anchor}\n#include {include}", 1)])

# nw4r's child lists find each pane from its link at the link's real offset, which the upstream fallback set to 0.
adapt("libs/nw4r/include/nw4r/lyt/pane.h", "include/nw4r/lyt/pane.h", [
    ("#ifdef __MWERKS__\n        typedef ut::LinkList< Pane, offsetof(detail::PaneBase, mLink) > PaneList;\n#else\n"
     "        typedef ut::LinkList< Pane, 0 > PaneList;\n#endif",
     "        typedef ut::LinkList< Pane, offsetof(detail::PaneBase, mLink) > PaneList;", 1),
])
# Lists find each element from its link at the link's real offset; upstream's non-Metrowerks fallback said 0, which
# under GCC is the vtable pointer the list would overwrite (AnimTransform) or another member.
adapt("libs/nw4r/include/nw4r/lyt/arcResourceAccessor.h", "include/nw4r/lyt/arcResourceAccessor.h", [
    ('            #ifdef __MWERKS__\n            typedef ut::LinkList<FontRefLink, offsetof(FontRefLink, mLink)>  FontRefList;\n            #else\n            typedef ut::LinkList<FontRefLink, 0> FontRefList;\n            #endif',
     '            typedef ut::LinkList<FontRefLink, offsetof(FontRefLink, mLink)>  FontRefList;', 1),
])
adapt("libs/nw4r/include/nw4r/lyt/group.h", "include/nw4r/lyt/group.h", [
    ('        #ifdef __MWERKS__\n        typedef ut::LinkList<detail::PaneLink, offsetof(detail::PaneLink, mLink)> PaneLinkList;\n        #else\n        typedef ut::LinkList<detail::PaneLink, 0>   PaneLinkList;\n        #endif',
     '        typedef ut::LinkList<detail::PaneLink, offsetof(detail::PaneLink, mLink)> PaneLinkList;', 1),
    ('        #ifdef __MWERKS__\n        typedef ut::LinkList<Group, offsetof(Group, mLink)> GroupList;\n        #else\n        typedef ut::LinkList<Group, 0> GroupList;\n        #endif',
     '        typedef ut::LinkList<Group, offsetof(Group, mLink)> GroupList;', 1),
])
adapt("libs/nw4r/include/nw4r/lyt/types.h", "include/nw4r/lyt/types.h", [
    ('#ifdef __MWERKS__\n        typedef ut::LinkList< AnimationLink, offsetof(AnimationLink, mLink) > AnimationList;\n#else\n        typedef ut::LinkList< AnimationLink, 0 > AnimationList;\n#endif',
     '        typedef ut::LinkList< AnimationLink, offsetof(AnimationLink, mLink) > AnimationList;', 1),
])
adapt("libs/nw4r/include/nw4r/lyt/layout.h", "include/nw4r/lyt/layout.h", [
    ('#ifdef __MWERKS__\n        typedef ut::LinkList< AnimTransform, offsetof(AnimTransform, mLink) > AnimTransformList;\n#else\n        typedef ut::LinkList< AnimTransform, 0 > AnimTransformList;\n#endif',
     '        typedef ut::LinkList< AnimTransform, offsetof(AnimTransform, mLink) > AnimTransformList;', 1),
])
adapt("libs/JSystem/include/JSystem/JUtility/JUTConsole.hpp", "include/JSystem/JUtility/JUTConsole.hpp", [
    ('#ifdef __MWERKS__\n    typedef JGadget::TLinkList< JUTConsole, -offsetof(JUTConsole, mListNode) > ConsoleList;\n#else\n    typedef JGadget::TLinkList< JUTConsole, 0 > ConsoleList;\n#endif',
     '    typedef JGadget::TLinkList< JUTConsole, -offsetof(JUTConsole, mListNode) > ConsoleList;', 1),
])
adapt("src/Game/Util/LayoutUtil.cpp", "Game/Util/LayoutUtil.cpp", [
    ("template const nw4r::lyt::Pane* nw4r::ut::LinkList< nw4r::lyt::Pane, 4 >::ConstIterator::operator->() const;",
     "template const nw4r::lyt::Pane* nw4r::lyt::PaneList::ConstIterator::operator->() const;", 1),
    ("template nw4r::lyt::PaneList::Iterator nw4r::ut::LinkList< nw4r::lyt::Pane, 4 >::Iterator::operator++(int);",
     "template nw4r::lyt::PaneList::Iterator nw4r::lyt::PaneList::Iterator::operator++(int);", 1),
    # The same 5:3 adjust scale as LayoutManager::calcAnim, for the galaxy map's capture.
    ('#include "Game/Util/LayoutUtil.hpp"\n', '#include "Game/Util/LayoutUtil.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("            pDrawInfo->mLocationAdjustScale = nw4r::math::VEC2(0.75f, 1.0f);",
     "            pDrawInfo->mLocationAdjustScale = nw4r::math::VEC2(smg::screen::LayoutAdjustX, 1.0f);", 1),
])

adapt("src/Game/AreaObj/ChangeBgmCube.cpp", "Game/AreaObj/ChangeBgmCube.cpp", [], cases=["        case 0:", "        case 1:"])
adapt("src/Game/System/GameEventFlagChecker.cpp", "Game/System/GameEventFlagChecker.cpp", [], cases=[
    "    case GameEventFlag::Type_GalaxyOpenStar:", "    case GameEventFlag::Type_EventFlag:",
    "    case GameEventFlag::Type_StarPiece:", "    case GameEventFlag::Type_EventValueIsZero:"])
adapt("src/Game/Animation/XanimeResource.cpp", "Game/Animation/XanimeResource.cpp", [], cases=["    case 1:"])
adapt("src/Game/Ride/SurfRay.cpp", "Game/Ride/SurfRay.cpp", [], cases=["    case 11:"])

# The Wii's entry point under a name the port's main calls, its phases timed for the perf line. A drawn step waits only
# for the frame before it (smgWaitFramesOut), so a frame draws through the next two steps, its draw syncs beside them.
adapt("src/Game/System/GameSystem.cpp", "Game/System/GameSystem.cpp", [
    ("void main(void) {\n    OSInitFastCast();", "void smgGameMain() {\n    OSInitFastCast();", 1),
    ('#include "Game/System/GameSystem.hpp"\n', '#include "Game/System/GameSystem.hpp"\n\nextern "C" void smgFramePhase(int phase);\nextern "C" int smgRenderInterval();\nextern "C" void smgAvSyncFrame();\nextern "C" void smgWaitFramesOut();\nextern "C" void smgFrameDrawn();\nextern "C" bool smgViewNeeded;\n', 1),
    ("void GameSystem::frameLoop() {\n    MainLoopFramework::sManager->beginRender();\n    draw();\n    MainLoopFramework::sManager->endRender();\n"
     "    update();\n    calcAnim();\n    mObjHolder->captureIfAllowForScreenPreserver();\n    MainLoopFramework::sManager->endFrame();\n"
     "    MainLoopFramework::sManager->waitForRetrace();\n}",
     "void GameSystem::frameLoop() {\n"
     "    MainLoopFramework::sManager->mRenderInterval = smgRenderInterval();\n"
     "    if (MainLoopFramework::sManager->mRenderCounter + 1 >= MainLoopFramework::sManager->mRenderInterval) {\n"
     "        smgFramePhase(3);\n        smgWaitFramesOut();\n    }\n"
     "    smgFramePhase(0);\n"
     "    MainLoopFramework::sManager->beginRender();\n    if (MainLoopFramework::sManager->mDoRenderFrame) {\n        draw();\n        smgAvSyncFrame();\n"
     "        smgFrameDrawn();\n    }\n"
     "    MainLoopFramework::sManager->endRender();\n"
     "    smgFramePhase(1);\n    update();\n"
     "    smgFramePhase(2);\n"
     "    smgViewNeeded = MainLoopFramework::sManager->mRenderCounter + 1 >= MainLoopFramework::sManager->mRenderInterval;\n"
     "    calcAnim();\n    smgViewNeeded = true;\n    mObjHolder->captureIfAllowForScreenPreserver();\n"
     "    smgFramePhase(3);\n    MainLoopFramework::sManager->endFrame();\n"
     "    smgFramePhase(4);\n    MainLoopFramework::sManager->waitForRetrace();\n}", 1),
])

# For development (settings.ini dev.galaxy=GALAXY N, off unless set): the first galaxy played after the file select is
# that one, scenario N. A hidden star's N plays the scenario it is placed in, as the scenario select chooses it.
adapt("src/Game/System/GameSequenceProgress.cpp", "Game/System/GameSequenceProgress.cpp", [
    ('#include "Game/System/GameSequenceProgress.hpp"\n',
     '#include "Game/System/GameSequenceProgress.hpp"\n\nextern "C" const char* smgDevGalaxy(int* scenario);  // source/compat/Kpad.cpp\n'
     'extern "C" int smgDevMarioStart(const char* galaxy);\n', 1),
    # A new file's stage is named only in moveGalaxy, which also queues the prologue: both dev settings stop the story so
    # its demos never meet another galaxy or a start point it never pairs them with.
    ("    mGalaxyCometScheduler->syncWithFlags();\n\n    updateGameDataBeforeChangeScene();\n",
     "    mGalaxyCometScheduler->syncWithFlags();\n\n    static bool sDevGalaxyTaken = false;\n    int devScenario = 0;\n"
     "    if (const char* devGalaxy = smgDevGalaxy(&devScenario); devGalaxy && !sDevGalaxyTaken && moveArg.mStageName != nullptr &&\n"
     "        !MR::isEqualString(moveArg.mStageName, \"FileSelect\")) {\n"
     "        OSReport(\"dev.galaxy: %s %ld instead of %s %ld\\n\", devGalaxy, (long)devScenario, moveArg.mStageName, (long)moveArg.mScenarioNo);\n"
     "        sDevGalaxyTaken = true;\n        moveArg.mStageName = devGalaxy;\n        moveArg.mScenarioNo = moveArg._C = devScenario;\n"
     "        if (MR::makeGalaxyStatusAccessor(devGalaxy).isHiddenStar(devScenario)) {\n"
     "            moveArg.mScenarioNo = MR::getPlacedHiddenStarScenarioNo(devGalaxy, devScenario);\n        }\n"
     "        mStorySequenceExecutor->forceStop();\n    }\n"
     "    if (moveArg.mStageName != nullptr && smgDevMarioStart(moveArg.mStageName) >= 0) {\n"
     "        mStorySequenceExecutor->forceStop();\n    }\n\n"
     "    updateGameDataBeforeChangeScene();\n", 1),
])

# For development (settings.ini dev.start=GALAXY N, off unless set): Mario starts that galaxy at its start point N.
adapt("src/Game/Scene/StageDataHolder.cpp", "Game/Scene/StageDataHolder.cpp", [
    ('#include "Game/Scene/StageDataHolder.hpp"\n',
     '#include "Game/Scene/StageDataHolder.hpp"\n#include "Game/System/GalaxyStatusAccessor.hpp"\n#include "Game/Util/SceneUtil.hpp"\n\n'
     'extern "C" int smgDevMarioStart(const char* galaxy);  // source/compat/Kpad.cpp\nextern "C" const char* smgDevMarioZone(const char* galaxy);\n', 1),
    ("    JMapIdInfo startID = MR::getCurrentMarioStartIdInfo();\n    JMapInfoIter iter = makeMarioJMapInfoIter(startID);",
     "    JMapIdInfo startID = MR::getCurrentMarioStartIdInfo();\n    if (const s32 start = smgDevMarioStart(MR::getCurrentStageName()); start >= 0) {\n"
     "        startID._0 = start;\n"
     "        if (const char* zone = smgDevMarioZone(MR::getCurrentStageName())) {\n"
     "            startID.mZoneID = MR::makeCurrentGalaxyStatusAccessor().getZoneId(zone);\n        }\n    }\n"
     "    JMapInfoIter iter = makeMarioJMapInfoIter(startID);", 1),
    # A placed object's zone is the holder one of whose tables it is in. The Wii told by address, the span from a
    # holder's lowest table to the end of its highest; here spans can overlap (Gateway's rails went to the wrong zone).
    ("    s32 data = (s32)rIter.mInfo->mData + rIter.mInfo->mData->mDataOffset + rIter.mInfo->mData->mEntrySize * rIter.mIndex;\n\n"
     "    if (_E4 <= data && data < _E8) {\n        return this;\n    }\n",
     "    for (const MR::AssignableArray< JMapInfo >* pArray : {&mPlacementObjs, &mStartObjs, &mGeneralPosObjs, &mChildObjs, &mPathObjs}) {\n"
     "        for (const JMapInfo* pInfo = pArray->begin(); pInfo != pArray->end(); pInfo++) {\n"
     "            if (pInfo->mData == rIter.mInfo->mData) {\n                return this;\n            }\n        }\n    }\n", 1),
])

# View matrices and material display lists a step makes are made again by the next before anything draws them when that
# step draws nothing, so the frame loop leaves them out there (smgViewNeeded). The material controllers set values,
# never add to them, so a step left out loses nothing.
adapt("src/Game/LiveActor/ModelManager.cpp", "Game/LiveActor/ModelManager.cpp", [
    ('#include "Game/LiveActor/ModelManager.hpp"\n', '#include "Game/LiveActor/ModelManager.hpp"\n\nextern "C" bool smgViewNeeded;  // source/compat/GameScene.cpp\n', 1),
    ("void ModelManager::calcView() {\n    getJ3DModel()->viewCalc();",
     "void ModelManager::calcView() {\n    if (!smgViewNeeded) {\n        return;\n    }\n\n    getJ3DModel()->viewCalc();", 1),
    ("void ModelManager::updateDL(bool isDiffDL) {\n    OSLockMutex",
     "void ModelManager::updateDL(bool isDiffDL) {\n    if (!smgViewNeeded) {\n        return;\n    }\n\n    OSLockMutex", 1),
])

# A scene is destroyed (on another thread) once the GPU is done with it: the Wii's frame loop had waited for each frame
# by then, but the render thread may still be drawing the last one, from the scene's own display lists and textures.
adapt("src/Game/System/GameSystemSceneController.cpp", "Game/System/GameSystemSceneController.cpp", [
    ('#include "Game/System/GameSystemSceneController.hpp"\n',
     '#include "Game/System/GameSystemSceneController.hpp"\n#include <revolution/gx.h>\n\nextern "C" void smgWaitDrawSyncs();\n', 1),
    ('        mIntermissionScene->setCurrentSceneControllerState("[destroy Scene: %s]", mCurrSceneControlInfo.mScene);\n',
     '        mIntermissionScene->setCurrentSceneControllerState("[destroy Scene: %s]", mCurrSceneControlInfo.mScene);\n'
     '        GXDrawDone();\n        smgWaitDrawSyncs();\n', 1),
])

# Metrowerks formed a pointer to member from the bare or unqualified name.
adapt("src/Game/System/ResourceHolderManager.cpp", "Game/System/ResourceHolderManager.cpp", [
    ("SingletonHolder< ResourceHolderManager >::get(), createResourceHolder,",
     "SingletonHolder< ResourceHolderManager >::get(), &ResourceHolderManager::createResourceHolder,", 1),
    ("SingletonHolder< ResourceHolderManager >::get(), createLayoutHolder,",
     "SingletonHolder< ResourceHolderManager >::get(), &ResourceHolderManager::createLayoutHolder,", 1),
    # A holder of an archive nobody mounted crashes on the Wii as here; the port names the archive first.
    ("    MR::getMountedArchiveAndHeap(pParam1, &pArchive, &pArgs->mHeap);\n    MR::CurrentHeapRestorer heapRestorer(pArgs->mHeap);\n\n"
     "    pArgs->mLayoutHolder = new LayoutHolder(*pArchive);",
     "    MR::getMountedArchiveAndHeap(pParam1, &pArchive, &pArgs->mHeap);\n"
     "    if (pArchive == nullptr) {\n        OSPanic(__FILE__, __LINE__, \"the layout archive %s is not mounted\", pParam1);\n    }\n"
     "    MR::CurrentHeapRestorer heapRestorer(pArgs->mHeap);\n\n    pArgs->mLayoutHolder = new LayoutHolder(*pArchive);", 1),
])
adapt("src/Game/Screen/GalaxyMapController.cpp", "Game/Screen/GalaxyMapController.cpp", [
    ("MR::Functor(this, GalaxyMapController::capture)", "MR::Functor(this, &GalaxyMapController::capture)", 1),
])
adapt("src/Game/NPC/NPCActor.cpp", "Game/NPC/NPCActor.cpp", [
    ("MR::createJointDelegatorWithNullChildFunc(this, &calcJointScale, rCaps._70)",
     "MR::createJointDelegatorWithNullChildFunc(this, &NPCActor::calcJointScale, rCaps._70)", 1),
])
# A built collision's one octree word is a leaf (the top bit) whose prism list follows two bytes on. The game writes it
# as two halves, 0x8000 then 2, which read back as that word only big-endian: the 3DS read a node 0x28000 bytes away.
adapt("src/Game/MapObj/DynamicCollisionObj.cpp", "Game/MapObj/DynamicCollisionObj.cpp", [
    ("    u16array[0] = 0x8000;\n    u16array[1] = 2;\n", "    *reinterpret_cast< s32* >(u16array) = static_cast< s32 >(0x80000002);\n", 1),
])
# Tested as a bare member function name, Metrowerks checked the pointer to member (__ptmf_test), which is never
# null: the condition always holds. Spelled out so GCC keeps that.
adapt("src/Game/MapObj/MapPartsRailPosture.cpp", "Game/MapObj/MapPartsRailPosture.cpp", [
    ("if (mMovePosture == 1 || isPostureTypeRailDirRailUseShadowGravity) {",
     "if (mMovePosture == 1 || &MapPartsRailPosture::isPostureTypeRailDirRailUseShadowGravity != nullptr) {", 1),
])
adapt("src/Game/MapObj/Sandstorm.cpp", "Game/MapObj/Sandstorm.cpp", [
    ("        if (isSunakazeKun) {", "        if (&Sandstorm::isSunakazeKun != nullptr) {", 1),
])
adapt("src/Game/Util/DemoUtil.cpp", "Game/Util/DemoUtil.cpp", [
    ("    LiveActor* getTalkingActor() {\n        if (isExistSceneObj(SceneObj_TalkDirector) == false) {\n            return false;",
     "    LiveActor* getTalkingActor() {\n        if (isExistSceneObj(SceneObj_TalkDirector) == false) {\n            return nullptr;", 1),
])
adapt("src/Game/Util/BezierSurface.cpp", "Game/Util/BezierSurface.cpp", [
    ("#include <revolution/gx/GXVert.h>", "#include <revolution/gx/GXGeometry.h>\n#include <revolution/gx/GXVert.h>", 1),
])
adapt("src/Game/MapObj/MapPartsBreaker.cpp", "Game/MapObj/MapPartsBreaker.cpp", [
    ("f32 TVec2f::squared(const TVec2f& rOther) const {", "template <>\nf32 TVec2f::squared(const TVec2f& rOther) const {", 1),
])
# A pointer to a temporary, read straight away by the caller: a local static holds the same value past the return.
# SimpleEffectObj's names a local instead, which gcc returns as null without a warning (the scan can't see it).
adapt("include/Game/Effect/SimpleEffectObj.hpp", "include/Game/Effect/SimpleEffectObj.hpp", [
    ("        TVec3f vec;\n        vec.set(0.0f, 0.0f, 0.0f);\n        return &vec;", "        static TVec3f offset(0.0f, 0.0f, 0.0f);\n        return &offset;", 1),
])
adapt("include/Game/MapObj/MorphItemObjNeo.hpp", "include/Game/MapObj/MorphItemObjNeo.hpp", [
    ("        return &(TVec3f(0.0f, 200.0f, 0.0f));", "        static TVec3f offset(0.0f, 200.0f, 0.0f);\n        return &offset;", 2),
    ("        return &TVec3f(0.0f, 580.0f, 0.0f);", "        static TVec3f offset(0.0f, 580.0f, 0.0f);\n        return &offset;", 1),
])
adapt("src/Game/Effect/EffectObjGravityDust.cpp", "Game/Effect/EffectObjGravityDust.cpp", [
    ("    return &(TVec3f(0.0f, 500.0f * mScale.y, 0.0f));",
     "    static TVec3f offset;\n    offset.set(0.0f, 500.0f * mScale.y, 0.0f);\n    return &offset;", 1),
])
adapt("src/Game/MapObj/PlantGroup.cpp", "Game/MapObj/PlantGroup.cpp", [
    ("        const TVec3f* pUp = &(rGravity * 100.0f);", "        const TVec3f up = rGravity * 100.0f;\n        const TVec3f* pUp = &up;", 1),
    ("        const TVec3f* pRay = &(rGravity * ::sCheckLineLength);",
     "        const TVec3f ray = rGravity * ::sCheckLineLength;\n        const TVec3f* pRay = &ray;", 1),
])
adapt("src/Game/NPC/TalkState.cpp", "Game/NPC/TalkState.cpp", TALK_STATE_NULLS + [
    ("f64 absolute = __fabs(camZ.dot(up));", "f64 absolute = std::fabs(camZ.dot(up));", 1),
])
adapt("src/Game/Boss/DinoPackunTailNode.cpp", "Game/Boss/DinoPackunTailNode.cpp", [
    # MSL's <cmath> M_PI is a float.
    ("MR::turnQuatXDirRad(&v24, v24, v23, M_PI);", "MR::turnQuatXDirRad(&v24, v24, v23, 3.14159265358979323846f);", 1),
])
adapt("src/Game/System/DrawBuffer.cpp", "Game/System/DrawBuffer.cpp", [
    # MSL declared strcasecmp in <cstring>; newlib and glibc do in <strings.h>.
    ("#include <cstring>", "#include <cstring>\n#include <strings.h>", 1),
])
# Our JMapInfo declares getValue returning plain bool (GCC ignores the const Metrowerks kept on it).
for relative, types in [("Game/LiveActor/DynamicJointCtrl.cpp", ["f32"]), ("Game/Demo/DemoActionKeeper.cpp", ["s32", "const char*"]),
                        ("Game/System/ScenarioDataParser.cpp", ["s32"])]:
    adapt("src/" + relative, relative, [(f"template const bool JMapInfo::getValue< {t} >", f"template bool JMapInfo::getValue< {t} >", 1)
                                        for t in types])
adapt("src/Game/Screen/ReplaceTagProcessor.cpp", "Game/Screen/ReplaceTagProcessor.cpp", [
    # MSL's va_list was a one-element array of its register-save struct; va_copy copies any toolchain's.
    ("        *copy = *args;\n", "        va_copy(copy, args);\n", 2),
])
adapt("src/Game/Scene/IntermissionScene.cpp", "Game/Scene/IntermissionScene.cpp", [
    ("    va_end();\n", "    va_end(list);\n", 1),
])
adapt("src/Game/System/MainLoopFramework.cpp", "Game/System/MainLoopFramework.cpp", [
    ("C_MTXOrtho(proj, 0f, efbHeight, 0f, fbWidth, 0f, 1f);", "C_MTXOrtho(proj, 0.0f, efbHeight, 0.0f, fbWidth, 0.0f, 1.0f);", 1),
    ("GXSetViewport(0f, 0f, fbWidth, efbHeight, 0f, 1f);", "GXSetViewport(0.0f, 0.0f, fbWidth, efbHeight, 0.0f, 1.0f);", 1),
    ("    if (mCombinationRatio > 1f) {\n        mCombinationRatio = 1f;", "    if (mCombinationRatio > 1.0f) {\n        mCombinationRatio = 1.0f;", 1),
    # Drawing every second step, GameSystem::frameLoop (adapted) waits for the drawn frame in the next step instead.
    ("            ::waitDrawDoneAndSetAlarm();\n            GXFlush();",
     "            if (mRenderInterval < 2) {\n                ::waitDrawDoneAndSetAlarm();\n            }\n            GXFlush();", 2),
    # The clear's Z24X8 texels are GPU data, so they keep the Wii's bytes: an AR block of 00 FF pairs, then GB of FF,
    # which is Z 0xFFFFFF. As little-endian words they read Z 0x00FFFF, near, and every frame cleared to it.
    ("u32 clearZTexData[] ATTRIBUTE_ALIGN(32) = {0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF, 0x00FF00FF,\n"
     "                                           0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};",
     "u8 clearZTexData[] ATTRIBUTE_ALIGN(32) = {0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,\n"
     "                                          0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,\n"
     "                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,\n"
     "                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};", 1),
])
# An explicit specialization has to come before the code that uses the member; Metrowerks took it after.
galaxy_map_destructors = "".join(f"template <>\ninline MR::Vector< MR::AssignableArray< {icon}* > >::~Vector() {{\n}}\n\n"
                                 for icon in ["GalaxyMapIcon", "GalaxyMapCometIcon", "GalaxyMapDomeIcon", "GalaxyMapTicoIcon"])
adapt("src/Game/Screen/GalaxyMap.cpp", "Game/Screen/GalaxyMap.cpp", [
    (galaxy_map_destructors, "", 1),
    ('#include "Game/System/GameDataConst.hpp"', '#include "Game/System/GameDataConst.hpp"\n#include <smg/DolTables.hpp>', 1),
    ("galaxyInfo.attach(&GalaxyIDBCSV);", 'galaxyInfo.attach(smg::dolTable("GalaxyIDBCSV"));', 1),
    ("GalaxyMap::GalaxyMap()\n", galaxy_map_destructors + "GalaxyMap::GalaxyMap()\n", 1),
])
demo_ring_buffer = (root / "src/Game/Demo/DemoStartRequestHolder.cpp").read_text()
demo_ring_buffer = demo_ring_buffer[demo_ring_buffer.index("template <>\nMR::FixedRingBuffer< const DemoStartInfo*, 16 >::iterator::iterator("):]
# push_back steps its iterator, so the iterator's operator++ goes first.
constructor, push_back, increment = ["template <>\n" + part for part in demo_ring_buffer.split("template <>\n")[1:]]
adapt("src/Game/Demo/DemoStartRequestHolder.cpp", "Game/Demo/DemoStartRequestHolder.cpp", [
    (demo_ring_buffer, "", 1),
    ("DemoStartInfo::DemoStartInfo() {\n", increment.rstrip("\n") + "\n\n" + constructor + push_back + "DemoStartInfo::DemoStartInfo() {\n", 1),
])

for relative, anchor, include in [
    ("libs/JSystem/include/JSystem/JKernel/JKRAramHeap.hpp", '#include "JSystem/JKernel/JKRDisposer.hpp"', "<revolution/os.h>"),
    ("libs/JSystem/include/JSystem/JKernel/JKRDvdFile.hpp", '#include "JSystem/JKernel/JKRDisposer.hpp"', "<revolution/dvd.h>\n#include <revolution/os.h>"),
    ("libs/JSystem/include/JSystem/JAudio2/JASAramStream.hpp", '#include "JSystem/JAudio2/JASTaskThread.hpp"', "<revolution/dvd.h>"),
    ("libs/JSystem/include/JSystem/J2DGraph/J2DPane.hpp", '#include "JSystem/JSupport/JSUList.hpp"', "<revolution/gx/GXEnum.h>"),
]:
    adapt(relative, relative[relative.index("include/"):], [(anchor, f"{anchor}\n#include {include}", 1)])
# The callback's argument carries a pointer (JAUSeqDataBlockMgr passes a link), which a 64-bit host cannot fit in u32.
adapt("libs/JSystem/include/JSystem/JAudio2/JASResArcLoader.hpp", "include/JSystem/JAudio2/JASResArcLoader.hpp", [
    ("typedef void (*LoadCallback)(u32, u32);", "typedef void (*LoadCallback)(u32, uintptr_t);", 1),
    ("/* 0x14 */ u32 mCallbackArg;", "/* 0x14 */ uintptr_t mCallbackArg;", 1),
    ("int loadResourceAsync(JKRArchive*, u16, u8*, u32, void (*)(u32, u32), u32);", "int loadResourceAsync(JKRArchive*, u16, u8*, u32, LoadCallback, uintptr_t);", 1),
])
# A DSP channel's six bus connections (setBusConnect, setMixerVolume: one row of four per bus) fill 0x10 to 0x40.
adapt("libs/JSystem/include/JSystem/JAudio2/JASDspInterface.hpp", "include/JSystem/JAudio2/JASDspInterface.hpp", [
    ("        /* 0x010 */ u16 _10[1][4];  // array size unknown\n        /* 0x018 */ u8 _18[0x050 - 0x018];",
     "        /* 0x010 */ u16 _10[6][4];\n        /* 0x040 */ u8 _40[0x050 - 0x040];", 1),
])
# A stream's first channel asks for bus 255 (JASAramStream::init's 0xffff), past the table: the Wii read 0x4861 from a
# string there, an address no mix buffer has, so it mixed nowhere; 0 connects nowhere too.
adapt("src/JSystem/JAudio2/JASDSPInterface.cpp", "JSystem/JAudio2/JASDSPInterface.cpp", [
    ("    r30 = connect_table[param_1];", "    r30 = param_1 < 12 ? connect_table[param_1] : 0;", 1),
])
# A heap inserted last names no next heap: the tree's null, without a member access through a null pointer.
adapt("src/JSystem/JAudio2/JASHeapCtrl.cpp", "JSystem/JAudio2/JASHeapCtrl.cpp", [
    ("    mTree.insertChild(&pNext->mTree, &pHeap->mTree);", "    mTree.insertChild(pNext ? &pNext->mTree : nullptr, &pHeap->mTree);", 1),
])
adapt("src/JSystem/JAudio2/JASResArcLoader.cpp", "JSystem/JAudio2/JASResArcLoader.cpp", [
    ("LoadCallback callback, u32 callbackArg)", "LoadCallback callback, uintptr_t callbackArg)", 1),
])
adapt("src/JSystem/JAudio2/JASSeqReader.cpp", "JSystem/JAudio2/JASSeqReader.cpp", [
    ("        mLoopCounts[i] = nullptr;", "        mLoopCounts[i] = 0;", 1),
])
adapt("src/JSystem/J2DGraph/J2DManage.cpp", "JSystem/J2DGraph/J2DManage.cpp", [("#include <cstdio>", "#include <cstdio>\n#include <cstring>", 1)])
# The port's JMath table holds TablePair, which has the a1/b1 names MSL's pair had.
adapt("src/JSystem/JParticle/JPAMath.cpp", "JSystem/JParticle/JPAMath.cpp", [
    ("const std::pair< f32, f32 >* p = &JMath::sSinCosTable", "const JMath::TablePair< f32, f32 >* p = &JMath::sSinCosTable", 2),
])
adapt("src/JSystem/JParticle/JPABaseShape.cpp", "JSystem/JParticle/JPABaseShape.cpp", [
    ("__declspec(force_export) static dirTypeFunc p_direction[5]", "static dirTypeFunc p_direction[5]", 1),
])
adapt("src/JSystem/JUtility/JUTException.cpp", "JSystem/JUtility/JUTException.cpp", [
    ("    panic_f_va(file, line, format, args);\n    va_end();", "    panic_f_va(file, line, format, args);\n    va_end(args);", 1),
    ("    if (isnan(value)) {", "    if (std::isnan(value)) {", 1), ("    } else if (isinf(value)) {", "    } else if (std::isinf(value)) {", 1),
])
# A JUTTexture's image is the GPU's own (the game reads it only by loading the texture), so its copies of the EFB stay
# in the PICA's memory instead of being encoded texel by texel on the CPU (docs/PORTING.md, "EFB copies on the GPU").
adapt("src/JSystem/JUtility/JUTTexture.cpp", "JSystem/JUtility/JUTTexture.cpp", [
    ('#include "JSystem/JUtility/JUTTexture.hpp"\n', '#include "JSystem/JUtility/JUTTexture.hpp"\n#include <smg/GxRender.hpp>\n', 1),
    ("        GXCopyTex(mImage, clear);\n        GXPixModeSync();",
     "        smg::gxRenderCopiesStayOnGpu(true);\n        GXCopyTex(mImage, clear);\n        smg::gxRenderCopiesStayOnGpu(false);\n"
     "        GXPixModeSync();", 1),
])
# VPanic walks the PowerPC back chain and takes over the Wii's video; the port's is in source/compat.
adapt("src/nw4r/db/db_assert.cpp", "nw4r/db/db_assert.cpp", [
    ("        __declspec(weak) void Panic(", "        void VPanic(const char*, int, const char*, va_list, bool);\n\n        void Panic(", 1),
], drop=["        __declspec(weak) void VPanic("], reason="the Wii's stack walk and video")
adapt("src/nw4r/lyt/lyt_arcResourceAccessor.cpp", "nw4r/lyt/lyt_arcResourceAccessor.cpp", [
    ("#include <extras.h>", "#include <cstring>\n#include <extras.h>", 1),
])

# What the scene libraries leave out of their copies, for the whole-game library (petari_game in CMakeLists.txt).
extract("src/Game/LiveActor/RailRider.cpp", "Game/LiveActor/RailRiderCamera.cpp",
        '#include "Game/LiveActor/RailRider.hpp"\n#include "Game/Util/JMapInfo.hpp"\n#include "Game/Util/JMapUtil.hpp"\n'
        '#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/SceneUtil.hpp"\n', ["RailRider::RailRider(s32 a1, s32 a2)"])
extract("src/Game/LiveActor/LiveActor.cpp", "Game/LiveActor/LiveActorSound.cpp",
        '#include "Game/LiveActor/LiveActor.hpp"\n#include "Game/AudioLib/AudAnmSoundObject.hpp"\n#include "Game/Util/MemoryUtil.hpp"\n',
        ["void LiveActor::initSound(int param1, bool is2D) {", "void LiveActor::addToSoundObjHolder() {"])
# Metrowerks emitted this NO_INLINE body for the other files that call it; GCC only does for a plain definition.
adapt("src/Game/Effect/ParticleEmitterHolder.cpp", "Game/Effect/ParticleEmitterHolder.cpp", [
    ("inline bool ParticleEmitter::isContinuousParticle() const NO_INLINE {", "bool ParticleEmitter::isContinuousParticle() const {", 1),
])
# SpinDriverPathDrawer writes the GPU FIFO directly; through the one write point it stops by name like every GX call.
adapt("src/Game/MapObj/SpinDriverPathDrawer.cpp", "Game/MapObj/SpinDriverPathDrawer.cpp", [
    (f"    GXWGFifo.f32 = {value};", f"    GXWGFIFO_PUT(f32, {value});", 1) for value in ["rA1.x", "rA1.y", "rA1.z", "a2", "a3"]
])
# Tables linked into main.dol rather than read from an archive; the port asks smg::dolTable for them by name.
dol_table = '#include <smg/DolTables.hpp>\n'
adapt("src/Game/System/GameEventFlagTable.cpp", "Game/System/GameEventFlagTable.cpp", [
    ("extern const JMapData GalaxyIDBCSV;\n", dol_table, 1),
    ("        info.attach(&GalaxyIDBCSV);", '        info.attach(smg::dolTable("GalaxyIDBCSV"));', 1),
])
adapt("src/Game/System/GalaxyNameSortTable.cpp", "Game/System/GalaxyNameSortTable.cpp", [
    ("extern const JMapData GalaxyIDBCSV;\n", dol_table, 1),
    ("    info.attach(&GalaxyIDBCSV);", '    info.attach(smg::dolTable("GalaxyIDBCSV"));', 1),
])
adapt("src/Game/System/GameDataHolder.cpp", "Game/System/GameDataHolder.cpp", [
    ("extern const u8 StoryEventBCSV[];\n", dol_table, 1),
    ("    mMapInfo->attach(StoryEventBCSV);", '    mMapInfo->attach(smg::dolTable("StoryEventBCSV"));', 1),
])
adapt("src/Game/System/GameDataConst.cpp", "Game/System/GameDataConst.cpp", [
    ('#include "Game/System/GameDataConst.hpp"', '#include "Game/System/GameDataConst.hpp"\n' + dol_table.rstrip("\n"), 1),
    ("    info.attach(&GalaxyIDBCSV);", '    info.attach(smg::dolTable("GalaxyIDBCSV"));', 2),
])
adapt("src/Game/Screen/GalaxyMapDomeIcon.cpp", "Game/Screen/GalaxyMapDomeIcon.cpp", [
    ('#include <cstdio>', '#include <cstdio>\n' + dol_table.rstrip("\n"), 1),
    ("    mapInfo.attach(&GalaxyIDBCSV);", '    mapInfo.attach(smg::dolTable("GalaxyIDBCSV"));', 2),
])

# Paired-single bodies Metrowerks alone compiled, with nothing for GCC: each gets the C++ of what its asm computes
# (tools/mwerks-only-asm.py finds them). Quantized casts clamp to the type's range and truncate, as Dolphin models them.
adapt("src/JSystem/J3DGraphBase/J3DShapeMtx.cpp", "JSystem/J3DGraphBase/J3DShapeMtx.cpp", [
    ("        stfs fr0, 0x20(destination)\n    }\n#endif", "        stfs fr0, 0x20(destination)\n    }\n#else\n"
     "    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) dst[row][col] = src[row][col];\n#endif", 1),
    ("        stfs v, 32(dst)\n    }\n#endif", "        stfs v, 32(dst)\n    }\n#else\n"
     "    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) dst[row][col] = src[row][col];\n#endif", 1),
])
adapt("src/JSystem/J3DGraphBase/J3DStruct.cpp", "JSystem/J3DGraphBase/J3DStruct.cpp", [
    ("    mEffectMtx[3][2] = kIdentityZero;\n    mEffectMtx[3][3] = kIdentityW;\n#endif",
     "    mEffectMtx[3][2] = kIdentityZero;\n    mEffectMtx[3][3] = kIdentityW;\n#else\n"
     "    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) mEffectMtx[row][col] = param_0[row][col];\n"
     "    mEffectMtx[3][0] = mEffectMtx[3][1] = mEffectMtx[3][2] = 0.0f;\n    mEffectMtx[3][3] = 1.0f;\n#endif", 1),
])
adapt("src/JSystem/J3DGraphBase/J3DSys.cpp", "JSystem/J3DGraphBase/J3DSys.cpp", [
    ("        mtspr   0x395, r3\n        }\n#endif", "        mtspr   0x395, r3\n        }\n#else\n"
     "    // The quantization registers belong to the Wii's paired-single unit; the port's casts are plain C++.\n#endif", 1),
])
adapt("libs/RVL_SDK/include/revolution/os/OSFastCast.h", "include/revolution/os/OSFastCast.h", [
    ('#define OSf32tou16(in, out) asm volatile("psq_st   %1, 0(%0), 1, 3 " : : "b"(out), "f"(*(in)) : "memory")\n#endif\n'
     '#define OSu16tof32(in, out) asm volatile("psq_l   %0, 0(%1), 1, 3  " : "=f"(*(out)) : "b"(in))',
     "static inline void OSf32tou16(const f32* in, volatile u16* out) {\n"
     "    *out = (u16)(*in < 0.0f ? 0.0f : *in > 65535.0f ? 65535.0f : *in);\n}\n#endif\n"
     "static inline void OSu16tof32(const u16* in, f32* out) {\n    *out = *in;\n}", 1),
    ("        mtspr   0x395, r3\n    }\n#endif", "        mtspr   0x395, r3\n    }\n#else\n"
     "    // The quantization registers belong to the Wii's paired-single unit; the port's casts are plain C++.\n#endif", 1),
])
adapt("libs/JSystem/include/JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp", "include/JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp", [
    ("            stfs v, 32(destination)\n        }\n#endif", "            stfs v, 32(destination)\n        }\n#else\n"
     "        for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) mpNrmMtxArr[1][mCurrentViewNo][idx][row][col] = mtx[row][col];\n#endif", 1),
])
adapt("libs/JSystem/include/JSystem/J3DGraphBase/J3DDrawBuffer.hpp", "include/JSystem/J3DGraphBase/J3DDrawBuffer.hpp", [
    ("    return out;\n#endif\n}", "    return out;\n#else\n    return m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];\n#endif\n}", 1),
])
adapt("src/JSystem/JMath/JMath.cpp", "JSystem/JMath/JMath.cpp", [
    ("        ps_sum0     dp, dp, dp, dp\n    }\n#endif", "        ps_sum0     dp, dp, dp, dp\n    }\n#else\n"
     "    dp = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;\n#endif", 1),
    ("        psq_st rz, 8(dst), 1, 0\n\t}\n#endif", "        psq_st rz, 8(dst), 1, 0\n\t}\n#else\n"
     "    dst->x = vec1->x * scale + vec2->x;\n    dst->y = vec1->y * scale + vec2->y;\n    dst->z = vec1->z * scale + vec2->z;\n#endif", 1),
    ("        stfs bz, 8(dst)\n    }\n#endif", "        stfs bz, 8(dst)\n    }\n#else\n"
     "    dst->x = (b->x - a->x) * t + a->x;\n    dst->y = (b->y - a->y) * t + a->y;\n    dst->z = (b->z - a->z) * t + a->z;\n#endif", 1),
])

# The GPU FIFO's register writes go to the port's FIFO, as GXVert.h's do.
adapt("libs/RVL_SDK/include/revolution/gx/GXRegs.h", "include/revolution/gx/GXRegs.h", [
    (f"#define GX_WRITE_{kind}({arg}) gxfifo.{field} = ({cast})({arg})", f"#define GX_WRITE_{kind}({arg}) smgGXFifo_{cast}(({cast})({arg}))", 1)
    for kind, arg, field, cast in [("U8", "ub", "u8", "u8"), ("U16", "us", "u16", "u16"), ("S16", "us", "s16", "u16"),
                                   ("U32", "ui", "u32", "u32"), ("F32", "f", "f32", "f32")]
] + [("#define MEM_PE_REQCOUNTH_IDX", "#include <smg/GxFifoWrite.h>\n\n#define MEM_PE_REQCOUNTH_IDX", 1),
      # A BP, CP or XF load is one command: its pieces go in with one look for room, the same bytes.
      ("#ifdef __MWERKS__\n#define GX_DEFINE_GX_READ_COUNTER(unit)",
       "#undef GX_WRITE_CP_STRM_REG\n#undef GX_WRITE_XF_REG\n#undef GX_WRITE_RA_REG\n"
       "#define GX_WRITE_CP_STRM_REG(addr, vtxfmt, data) smgGXFifoPut3(CP_OPCODE(0, 1), 1, CP_STREAM_REG((vtxfmt), (addr)), 1, (u32)(data), 4)\n"
       "#define GX_WRITE_XF_REG(addr, data, cnt) smgGXFifoPut3(CP_OPCODE(0, 0x2), 1, CP_XF_LOADREGS((addr), (cnt)), 4, (u32)(data), 4)\n"
       "#define GX_WRITE_RA_REG(reg) smgGXFifoPut2(CP_OPCODE(1, 0xC), 1, (u32)(reg), 4)\n"
       "#define GX_WRITE_RA_REG2(first, second) smgGXFifoPut4(CP_OPCODE(1, 0xC), 1, (u32)(first), 4, CP_OPCODE(1, 0xC), 1, (u32)(second), 4)\n\n"
       "#ifdef __MWERKS__\n#define GX_DEFINE_GX_READ_COUNTER(unit)", 1),
      # Upstream's fallbacks for other compilers are empty, fit only for an editor's parser: the counters read
      # nothing and FAST_FLAG_SET, which packs every register's fields, does nothing. GCC gets real ones:
      # Metrowerks' counter reader, and rlwimi's insert under its mask in C.
      ("#ifdef __MWERKS__\n#define GX_DEFINE_GX_READ_COUNTER(unit)", "#if 1\n#define GX_DEFINE_GX_READ_COUNTER(unit)", 1),
      ("#else\n#define GX_CP_COUNTER_READ_U32(name)\n#define GX_PE_COUNTER_READ_U32(name)\n#define GX_MEM_COUNTER_READ_U32(name)\n"
       "#define FAST_FLAG_SET(regOrg, newFlag, shift, size)\n#endif",
       "#else\n#define GX_CP_COUNTER_READ_U32(name) __GXReadCPCounterU32(name##L, name##H)\n"
       "#define GX_PE_COUNTER_READ_U32(name) __GXReadPECounterU32(name##L_ID, name##H_ID)\n"
       "#define GX_MEM_COUNTER_READ_U32(name) __GXReadMEMCounterU32(name##L_IDX, name##H_IDX)\n"
       "#define FAST_FLAG_SET(regOrg, newFlag, shift, size)                                                  \\\n"
       "    do {                                                                                              \\\n"
       "        const u32 fastFlagMask = (u32)(((1ULL << (size)) - 1) << (shift));                            \\\n"
       "        (regOrg) = ((u32)(regOrg) & ~fastFlagMask) | (((u32)(newFlag) << (shift)) & fastFlagMask);  \\\n"
       "    } while (0);\n#endif", 1)])
adapt("libs/RVL_SDK/include/revolution/gx/GXFDLShortcut.h", "include/revolution/gx/GXFDLShortcut.h", [
    ("#else\n#define FAST_GPFLAGSET(regOrg, newFlag, regName)\n#endif",
     "#else\n// rlwimi's insert under the field's mask, in C (upstream's fallback here is empty).\n"
     "#define FAST_GPFLAGSET(regOrg, newFlag, regName)                                                                 \\\n"
     "    do {                                                                                                          \\\n"
     "        const u32 fastFlagMask = (u32)(((1ULL << (regName##_SIZE)) - 1) << (regName##_SHIFT));                     \\\n"
     "        (regOrg) = ((u32)(regOrg) & ~fastFlagMask) | (((u32)(newFlag) << (regName##_SHIFT)) & fastFlagMask);     \\\n"
     "    } while (0)\n#endif", 1),
])
# JKRAram.hpp declared JKRDecomp's inline helper as an ordinary function, which Metrowerks then emitted somewhere.
adapt("libs/JSystem/include/JSystem/JKernel/JKRAram.hpp", "include/JSystem/JKernel/JKRAram.hpp", [
    ("u32 JKRDecompExpandSize(u8*);", '#include "JSystem/JKernel/JKRDecomp.hpp"', 1),
])
# Petari's file select sets a Mii-name font from MiiFont.arc, which Jaxi's US disc doesn't have and its main.dol
# never names: there the names keep their layouts' own fonts.
# The port shows nothing about Miis (Jaxi, 2026-10-01; its RFL is an empty Mii database): a save whose icon is a Mii
# (one from a Wii) takes Mario's icon without System_FileSelect009's "The Mii that you were using does not appear...".
adapt("src/Game/Map/FileSelector.cpp", "Game/Map/FileSelector.cpp", [
    ('    JKRMemArchive* pArchive = MR::receiveArchive("/LayoutData/MiiFont.arc");\n',
     '    JKRMemArchive* pArchive = MR::receiveArchive("/LayoutData/MiiFont.arc");\n    if (pArchive == nullptr) {\n        return;\n    }\n', 1),
    ('    if (MR::isFirstStep(this)) {\n        mSysInfoWindowMini->appear("System_FileSelect009", SysInfoWindow::Type_Key, SysInfoWindow::TextPos_Center, SysInfoWindow::MessageType_System);\n    }\n\n    if (MR::isDead(mSysInfoWindowMini)) {\n        setNerve(GET_NERVE(FileSelector, FileSelectorNrvFileConfirmMiiDeleteSave));\n    }\n',
     '    setNerve(GET_NERVE(FileSelector, FileSelectorNrvFileConfirmMiiDeleteSave));\n', 1),
])
# Jaxi's call (2026-10-01): in stylus mode the "Point at the screen with [Wii Remote]" banner never appears; there it
# would show every time the stylus is lifted. Other modes keep it, and the blue star's guidance keeps its timing.
adapt("src/Game/Screen/StarPointerGuidance.cpp", "Game/Screen/StarPointerGuidance.cpp", [
    ('#include "Game/Screen/StarPointerGuidance.hpp"\n',
     '#include "Game/Screen/StarPointerGuidance.hpp"\n\nextern "C" BOOL smgPointerIsStylus();  // source/compat/Kpad.cpp\n', 1),
    ("            if (mSpineFrame1P->isCurrentNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvEnd1P)) ||\n"
     "                mSpineFrame1P->isCurrentNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvEndWait))) {\n"
     "                mSpineFrame1P->setNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvAppear1P));\n",
     "            if (!smgPointerIsStylus() && (mSpineFrame1P->isCurrentNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvEnd1P)) ||\n"
     "                                          mSpineFrame1P->isCurrentNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvEndWait)))) {\n"
     "                mSpineFrame1P->setNerve(GET_NERVE(StarPointerGuidance, HostTypeNrvAppear1P));\n", 1),
])
# For development (settings.ini dev.skip_story=1, off unless set): the prologue's picture book ends as it appears, and any
# movie takes the skip the final battle's has once Bowser's third battle is won, so a boot reaches gameplay quickly.
adapt("src/Game/Screen/ProloguePictureBook.cpp", "Game/Screen/ProloguePictureBook.cpp", [
    ('#include "Game/Screen/ProloguePictureBook.hpp"\n',
     '#include "Game/Screen/ProloguePictureBook.hpp"\n\nextern "C" BOOL smgDevSkipStory();  // source/compat/Kpad.cpp\n', 1),
    ('    MR::startAnim(this, "Prologue", 0);\n    setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookPlaying));\n',
     '    MR::startAnim(this, "Prologue", 0);\n    if (smgDevSkipStory()) {\n        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookEnd));\n'
     '    } else {\n        setNerve(GET_NERVE(ProloguePictureBook, ProloguePictureBookPlaying));\n    }\n', 1),
])
adapt("src/Game/Screen/MoviePlayingSequence.cpp", "Game/Screen/MoviePlayingSequence.cpp", [
    ('#include "Game/Screen/MoviePlayingSequence.hpp"\n',
     '#include "Game/Screen/MoviePlayingSequence.hpp"\n\nextern "C" BOOL smgDevSkipStory();  // source/compat/Kpad.cpp\n', 1),
    ('    if (!MR::isEqualStringCase(mInfo->mMovieName, "/MovieData/FinalBattle.thp")) {\n        return false;\n    }\n\n'
     '    if (!MR::isAlreadyVisitedStage("KoopaBattleVs3Galaxy")) {\n        return false;\n    }\n\n'
     '    if (!MR::testCorePadButtonA(WPAD_CHAN0)) {\n        return false;\n    }\n',
     '    if (!smgDevSkipStory()) {\n'
     '        if (!MR::isEqualStringCase(mInfo->mMovieName, "/MovieData/FinalBattle.thp")) {\n            return false;\n        }\n\n'
     '        if (!MR::isAlreadyVisitedStage("KoopaBattleVs3Galaxy")) {\n            return false;\n        }\n\n'
     '        if (!MR::testCorePadButtonA(WPAD_CHAN0)) {\n            return false;\n        }\n    }\n', 1),
])
# Nor does the icon picker end with a "Mii" placeholder when there are no Miis (choosing it said "If you create a Mii in
# the Mii Channel, you can use it as your icon!"): the slot stays empty like any past the last icon.
adapt("src/Game/Screen/MiiSelect.cpp", "Game/Screen/MiiSelect.cpp", [
    ("            } else if (baseIndex + i == iconNum) {\n                mIconArray[i]->appearMiiDummy();\n                continue;\n"
     "            } else if (baseIndex + i > iconNum) {\n",
     "            } else if (baseIndex + i >= iconNum) {\n", 1),
])
# A BDL's material display lists carry texture numbers in their image-pointer BP commands, which J3D reads back from
# the list: big-endian GPU data, so the number is its three data bytes in that order.
adapt("src/JSystem/J3DGraphBase/J3DTevs.cpp", "JSystem/J3DGraphBase/J3DTevs.cpp", [
    ("    u32 var_r31 = *(u32*)((u8*)pDL + 1);\n    return var_r31 & 0xFFFFFF;",
     "    const u8* command = static_cast< const u8* >(pDL);\n    return u16(command[3] << 8 | command[4]);", 1),
])
# A streamed track's headers turn native as they come off the disc (compat/smg/NativeResources.hpp).
adapt("src/JSystem/JAudio2/JASAramStream.cpp", "JSystem/JAudio2/JASAramStream.cpp", [
    ('#include "JSystem/JAudio2/JASAramStream.hpp"\n', '#include "JSystem/JAudio2/JASAramStream.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("    DCInvalidateRange(sReadBuffer, sizeof(Header));\n", "    DCInvalidateRange(sReadBuffer, sizeof(Header));\n    smgNativeAstHeader(sReadBuffer);\n", 1),
    ("    DCInvalidateRange(sReadBuffer, size);\n    BlockHeader* bhead", "    DCInvalidateRange(sReadBuffer, size);\n    smgNativeAstBlock(sReadBuffer);\n    BlockHeader* bhead", 1),
])
# A sound ID's composite word is (section << 24 | group << 16 | wave), as the game builds and stores it; the parts
# overlaying it are ordered by the CPU's byte order so they read the same fields on ARM.
adapt("libs/JSystem/include/JSystem/JAudio2/JAISound.hpp", "include/JSystem/JAudio2/JAISound.hpp", [
    ("        struct {\n            union {\n                u16 value;\n                struct {\n                    u8 sectionID;\n                    u8 groupID;\n"
     "                } parts;\n            } type;\n            u16 waveID;\n        } info;",
     "        struct {\n#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n            u16 waveID;\n            union {\n                u16 value;\n"
     "                struct {\n                    u8 groupID;\n                    u8 sectionID;\n                } parts;\n            } type;\n#else\n"
     "            union {\n                u16 value;\n                struct {\n                    u8 sectionID;\n                    u8 groupID;\n"
     "                } parts;\n            } type;\n            u16 waveID;\n#endif\n        } info;", 1),
])
# The Wii waits out IR noise: a pointer shows after 5 valid samples and lingers 10 after losing the sensor bar. The
# 3DS's pointer is exact (touch, gyro or stick), so it shows at once and a lifted stylus hides it at once.
adapt("src/Game/System/WPadPointer.cpp", "Game/System/WPadPointer.cpp", [
    ("    static const s32 sIsValidInScreenTime = 5;", "    static const s32 sIsValidInScreenTime = 0;", 1),
    ("    static const s32 sIsInValidOutScreenTime = 10;", "    static const s32 sIsInValidOutScreenTime = 0;", 1),
])
# Eight of these run each logic step (a remote and a Nunchuk on each of four channels, three with nothing there), each
# averaging and turning over its last 128 samples. Those read nothing else, so while the 128 are one value (no
# controller, or a remote held still) they're kept as they were: the same results without the loops.
adapt("include/Game/System/WPadAcceleration.hpp", "include/Game/System/WPadAcceleration.hpp", [
    ("    void updateIsStable();\n", "    void updateIsStable();\n    void notePush();\n", 1),
    ("    /* 0x64C */ s32 _64C;\n", "    /* 0x64C */ s32 _64C;\n    s32 mSameRun;          // samples in a row equal to the one before\n"
     "    bool mWindowChanged;  // this update changed the last 128\n", 1),
])
adapt("src/Game/System/WPadAcceleration.cpp", "Game/System/WPadAcceleration.cpp", [
    ("#include <revolution/wpad.h>\n", "#include <revolution/wpad.h>\n#include <cstring>\n", 1),
    ("      _648(), _64C() {", "      _648(), _64C(), mSameRun(), mWindowChanged() {", 1),
    ("    s32 statusCount = mPad->getValidStatusCount();\n", "    s32 statusCount = mPad->getValidStatusCount();\n    mWindowChanged = false;\n", 1),
    ("        mHistory[_624] = previous;\n", "        mHistory[_624] = previous;\n        notePush();\n", 1),
    ("        mHistory[_624] = acceleration;\n", "        mHistory[_624] = acceleration;\n        notePush();\n", 1),
    ("    _62C.zero();\n\n    for (s32 j = 0; j < _628; j++) {",
     "    if (_628 >= 128 && !mWindowChanged) {\n        updateIsStable();\n        return;\n    }\n\n    _62C.zero();\n\n"
     "    for (s32 j = 0; j < _628; j++) {", 1),
    ("void WPadAcceleration::updateIsStable() {",
     "// Before _628 counts the sample just written: a run past 128 means the last 128 were already that one value.\n"
     "void WPadAcceleration::notePush() {\n"
     "    const s32 before = _624 == 0 ? 127 : _624 - 1;\n"
     "    const bool same = _628 > 0 && !std::memcmp(&mHistory[_624], &mHistory[before], sizeof(TVec3f));\n"
     "    mSameRun = same ? (mSameRun < 1000 ? mSameRun + 1 : mSameRun) : 1;\n"
     "    mWindowChanged = mWindowChanged || mSameRun <= 128;\n"
     "}\n\n"
     "void WPadAcceleration::updateIsStable() {", 1),
])
# A mix configuration is written as a word (its bus in the high byte, then two nibbles) and read through bitfields,
# which Metrowerks lays out from the word's first byte and GCC on ARM from its low end: here they name the same bits.
adapt("libs/JSystem/include/JSystem/JAudio2/JASChannel.hpp", "include/JSystem/JAudio2/JASChannel.hpp", [
    ("        struct {\n            u8 upper : 8;\n            u8 lower0 : 4;\n            u8 lower1 : 4;\n        };",
     "        struct {\n#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n            u8 lower1 : 4;\n            u8 lower0 : 4;\n"
     "            u8 upper : 8;\n#else\n            u8 upper : 8;\n            u8 lower0 : 4;\n            u8 lower1 : 4;\n#endif\n        };", 1),
])
# No Wii Remote strap reminder (Jaxi's call: the 3DS has no strap): the logo scene goes straight to its wait for the
# system archive, and the strap's layout is never made.
adapt("src/Game/Scene/LogoScene.cpp", "Game/Scene/LogoScene.cpp", [
    ("        initNerve(GET_NERVE_ANON(LogoSceneStrapFadein));", "        initNerve(GET_NERVE_ANON(LogoSceneWaitReadDoneSystemArchive));", 1),
    ("    if (tryFadeoutLayout()) {\n        setNerve(GET_NERVE_ANON(LogoSceneStrapFadein));",
     "    if (tryFadeoutLayout()) {\n        setNerve(GET_NERVE_ANON(LogoSceneWaitReadDoneSystemArchive));", 1),
    ('    mStrapLayout = MR::createSimpleLayout("ストラップ着用画面", "WiiRemoteStrap", 1);\n    mStrapLayout->kill();\n', "", 1),
    # The Chinese ISBN screen's layout adjusts to the 5:3 screen as every other layout does.
    ('#include "Game/Scene/LogoScene.hpp"\n', '#include "Game/Scene/LogoScene.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("        mIsbnManager->setAdjustRate(0.75f, 1.0f);", "        mIsbnManager->setAdjustRate(smg::screen::LayoutAdjustX, 1.0f);", 1),
])
# DspBoot hands the DSP its microcode at cached-mirror addresses (0x80000000 up), which no 3DS image can hold; on
# the 3DS every view of memory is the address itself (source/compat/WiiAudio.cpp runs the DSP).
adapt("src/JSystem/JAudio2/dsptask.cpp", "JSystem/JAudio2/dsptask.cpp", [
    ("    audiotask.iram_mmem_addr = (u16*)((u8*)jdsp + 0x80000000);", "    audiotask.iram_mmem_addr = jdsp;", 1),
    ("    audiotask.dram_mem_addr = (u16*)((u8*)AUDIO_YIELD_BUFFER + 0x80000000);", "    audiotask.dram_mem_addr = AUDIO_YIELD_BUFFER;", 1),
    # The DSP's interrupt reaches the game through other threads here, which a spin of higher priority would starve.
    ("        firstWarning = FALSE;\n    }", "        firstWarning = FALSE;\n        OSSleepTicks(OSMicrosecondsToTicks(100));\n    }", 1),
])
# Metrowerks' size_t is u32's unsigned long; the toolchains' own size_t is what operator new takes. The Wii's
# boot info sits at the start of MEM1; the port's is in source/compat/OsMemory.cpp.
# JSystem's getSize for any block, which the decomp declares but never defines, as JKRHeap::free finds its heap.
adapt("src/JSystem/JKernel/JKRHeap.cpp", "JSystem/JKernel/JKRHeap.cpp", [
    ("JKRHeap* JKRHeap::findFromRoot(void* pData) {",
     "s32 JKRHeap::getSize(void* pData, JKRHeap* pHeap) {\n    if (!pHeap) {\n        pHeap = findFromRoot(pData);\n    }\n"
     "    return pHeap ? pHeap->do_getSize(pData) : -1;\n}\n\nJKRHeap* JKRHeap::findFromRoot(void* pData) {", 1),
    ("void* operator new(u32 size", "void* operator new(size_t size", 3), ("void* operator new[](u32 size", "void* operator new[](size_t size", 3),
    ("    OSBootInfo* code = (OSBootInfo*)OSPhysicalToCached(0);", "    OSBootInfo* code = smgBootInfo();", 1),
    ("bool JKRHeap::initArena(char** memory, u32* size, int maxHeaps) {",
     'extern "C" OSBootInfo* smgBootInfo();\n\nbool JKRHeap::initArena(char** memory, u32* size, int maxHeaps) {', 1),
    # A heap made without an error handler fails an allocation silently; the port names each one in the log.
    ("void* JKRHeap::alloc(u32 size, int align) {\n    return do_alloc(size, align);\n}",
     "void* JKRHeap::alloc(u32 size, int align) {\n    void* memory = do_alloc(size, align);\n"
     "    if (memory == nullptr) {\n"
     "        OSReport(\"JKRHeap %p could not give %lu bytes aligned %d (%lu free, %lu in one block)\\n\", this, (unsigned long)size, align,\n"
     "                 (unsigned long)getTotalFreeSize(), (unsigned long)getFreeSize());\n    }\n    return memory;\n}", 1),
])
# The message file turns native where MessageData first reads it (compat/smg/NativeResources.hpp).
adapt("src/Game/System/MessageHolder.cpp", "Game/System/MessageHolder.cpp", [
    ('#include <JSystem/JKernel/JKRArchive.hpp>\n', '#include <JSystem/JKernel/JKRArchive.hpp>\n#include <smg/NativeResources.hpp>\n', 1),
    ('    u8* msgData = (u8*)pArchive->getResource("Message.bmg");\n',
     '    u8* msgData = (u8*)pArchive->getResource("Message.bmg");\n    smgNativeBmg(msgData, pArchive->getResSize(msgData));\n', 1),
])
# A string tag's slot holds a pointer the game stored natively; the Wii's first byte of it was the top byte.
adapt("src/Game/Screen/CustomTagProcessor.cpp", "Game/Screen/CustomTagProcessor.cpp", [
    ("    if (!*reinterpret_cast< const u8* >(tag.getParamPtr(0))) {",
     "    if (!(reinterpret_cast< uintptr_t >(*reinterpret_cast< const wchar_t* const* >(tag.getParamPtr(0))) >> 24)) {", 1),
])
# On ARM the header test only answers false for the disc's big-endian layouts, and the game walks the unbuilt one.
# Every visible pane of every layout works out its matrix each logic step: a rotation and a product per axis. An axis
# turned by nothing (most of the HUD's panes) leaves the matrix as it was, so it's copied instead.
adapt("src/nw4r/lyt/lyt_pane.cpp", "nw4r/lyt/lyt_pane.cpp", [
    ("""            PSMTXRotRad(rotateMtx, 'x', NW4R_MATH_DEG_TO_RAD(mRotate.x));
            PSMTXConcat(rotateMtx, mtx2, mtx1);

            PSMTXRotRad(rotateMtx, 'y', NW4R_MATH_DEG_TO_RAD(mRotate.y));
            PSMTXConcat(rotateMtx, mtx1, mtx2);

            PSMTXRotRad(rotateMtx, 'z', NW4R_MATH_DEG_TO_RAD(mRotate.z));
            PSMTXConcat(rotateMtx, mtx2, mtx1);""", """            if (mRotate.x != 0.0f) {
                PSMTXRotRad(rotateMtx, 'x', NW4R_MATH_DEG_TO_RAD(mRotate.x));
                PSMTXConcat(rotateMtx, mtx2, mtx1);
            } else {
                PSMTXCopy(mtx2, mtx1);
            }

            if (mRotate.y != 0.0f) {
                PSMTXRotRad(rotateMtx, 'y', NW4R_MATH_DEG_TO_RAD(mRotate.y));
                PSMTXConcat(rotateMtx, mtx1, mtx2);
            } else {
                PSMTXCopy(mtx1, mtx2);
            }

            if (mRotate.z != 0.0f) {
                PSMTXRotRad(rotateMtx, 'z', NW4R_MATH_DEG_TO_RAD(mRotate.z));
                PSMTXConcat(rotateMtx, mtx2, mtx1);
            } else {
                PSMTXCopy(mtx2, mtx1);
            }""", 1),
])
adapt("src/nw4r/lyt/lyt_layout.cpp", "nw4r/lyt/lyt_layout.cpp", [
    ('#include "nw4r/lyt/util.h"\n', '#include "nw4r/lyt/util.h"\n#include <revolution/os.h>\n#include <cstring>\n', 1),
    ("            if (!detail::TestFileHeader(*pFileHead, 'RLYT')) {\n",
     "            if (!std::memcmp(lytResBuf, \"RLYT\\xFE\\xFF\", 6)) {\n"
     "                OSPanic(__FILE__, __LINE__, \"a BRLYT reached nw4r still big-endian: layouts turn native as LayoutHolder mounts them\");\n"
     "            }\n"
     "            if (!detail::TestFileHeader(*pFileHead, 'RLYT')) {\n", 1),
])
# A BRLAN any other way than through LayoutHolder would be skipped silently by the header test.
adapt("src/nw4r/lyt/lyt_animation.cpp", "nw4r/lyt/lyt_animation.cpp", [
    ("#include <new>\n", "#include <new>\n#include <cstring>\n", 1),
    ("            if (!detail::TestFileHeader(*pFileHeader, res::FILESIGNATURE_RLAN)) {\n",
     "            if (!std::memcmp(anmResBuf, \"RLAN\\xFE\\xFF\", 6)) {\n"
     "                OSPanic(__FILE__, __LINE__, \"a BRLAN reached nw4r still big-endian: animations turn native as LayoutHolder mounts them\");\n"
     "            }\n"
     "            if (!detail::TestFileHeader(*pFileHeader, res::FILESIGNATURE_RLAN)) {\n", 1),
])
# Fonts come from GameSystemFontHolder's archives rather than a layout holder: they turn native as ResFont takes them.
adapt("src/nw4r/ut/ut_ResFont.cpp", "nw4r/ut/ut_ResFont.cpp", [
    ('#include "nw4r/ut/binaryFileFormat.h"\n', '#include "nw4r/ut/binaryFileFormat.h"\n#include <smg/NativeResources.hpp>\n', 1),
    ("            BinaryFileHeader* fileHeader = reinterpret_cast< BinaryFileHeader* >(brfnt);\n",
     "            smgNativeFont(brfnt);\n            BinaryFileHeader* fileHeader = reinterpret_cast< BinaryFileHeader* >(brfnt);\n", 1),
])
# TPL's headers hold GX enums, 4 bytes under Metrowerks' -enum int and 1 under devkitARM's short enums; the file's
# fields are spelled as the words they are.
adapt("libs/RVL_SDK/include/revolution/tpl.h", "include/revolution/tpl.h", [
    ("    GXTlutFmt format;\n    char* data;\n} TPLClutHeader", "    u32 format;\n    char* data;\n} TPLClutHeader", 1),
    ("    GXTexWrapMode wrapS;\n    GXTexWrapMode wrapT;\n    GXTexFilter minFilter;\n    GXTexFilter magFilter;",
     "    u32 wrapS;\n    u32 wrapT;\n    u32 minFilter;\n    u32 magFilter;", 1),
])
# A palette not yet bound holds offsets from itself, a bound one pointers past it; the Wii told them apart by the
# 0x80000000 its pointers all sit above, which no 3DS address reaches.
adapt("src/nw4r/lyt/lyt_texMap.cpp", "nw4r/lyt/lyt_texMap.cpp", [
    ("SetPaletteFormat(pClut->format);", "SetPaletteFormat(GXTlutFmt(pClut->format));", 1),
    ("            if (reinterpret_cast< u32 >(p->descriptorArray) < 0x80000000) {",
     "            if (reinterpret_cast< uintptr_t >(p->descriptorArray) < reinterpret_cast< uintptr_t >(p)) {", 1),
])
# The Wii told a main-memory address from an ARAM offset by its value (0x80000000 up), and every 3DS address is
# below that: main memory is the source exactly when copying into ARAM (direction 0; main.dol's own code agrees).
# captureDolTexture's image is a texture the game only draws with (MarioShadow's), so its copy stays on the GPU as
# JUTTexture::capture's does.
adapt("src/Game/System/Overwrite.cpp", "Game/System/Overwrite.cpp", [
    ('#include "Game/System/ShapePacketUserData.hpp"\n', '#include "Game/System/ShapePacketUserData.hpp"\n#include <smg/GxRender.hpp>\n', 1),
    ("    if (pCommand->mSrc < 0x80000000) {", "    if (pCommand->mTransferDirection != 0) {", 1),
    ("    GXSetTexCopyDst(width, height, format, mipmap);\n    GXCopyTex(pBuffer, GX_FALSE);",
     "    GXSetTexCopyDst(width, height, format, mipmap);\n    smg::gxRenderCopiesStayOnGpu(true);\n    GXCopyTex(pBuffer, GX_FALSE);\n"
     "    smg::gxRenderCopiesStayOnGpu(false);", 1),
])
# FIFO pointers share this queue with draw-sync tokens (below 0x10000) and the stop message (0x10000). The Wii's
# pointers sat above 0x80000000; the 3DS's sit above 0x10000.
adapt("src/Game/System/DrawSyncManager.cpp", "Game/System/DrawSyncManager.cpp", [
    ("        if (reinterpret_cast< u32 >(message) >= 0x80000000) {", "        if (reinterpret_cast< u32 >(message) > 0x10000) {", 1),
])
# The Wii's MEM1 is writable wherever the program put this, and archives turn native in place as they mount; a 3dsx's
# read-only data really is read-only.
adapt("include/Game/System/ErrorArchive.hpp", "include/Game/System/ErrorArchive.hpp", [
    ("const u8 cErrorArchive[] = {", "alignas(32) inline u8 cErrorArchive[] = {", 1),
])
adapt("src/RVL_SDK/mem/mem_allocator.c", "RVL_SDK/mem/mem_allocator.c", [], drop=[
    "static void* AllocatorAllocForExpHeap_(MEMAllocator *pAllocator, u32 size) {",
    "static void AllocatorFreeForExpHeap_(MEMAllocator *pAllocator, void *pBlock) {",
    "void MEMInitAllocatorForExpHeap(MEMAllocator *pAllocator, MEMHeapHandle handle, int align) {",
], reason="the SDK's expanded heaps, which the game never makes: it gives nw4r its own allocators")
# The Yaz0 header's decoded size is a big-endian word, as this file's other path already reads it.
adapt("src/Game/System/FileRipper.cpp", "Game/System/FileRipper.cpp", [
    ("    u32 data_len = *(u32*)(src + 4);", "    u32 data_len = (src[4] << 24) | (src[5] << 16) | (src[6] << 8) | src[7];", 1),
])
# On the Wii the audio thread, above its creator on one core, spun through its whole start-up before the creator ran
# again; on the 3DS its start-up blocks (NDSP), so the creator waits for it, as the game's reverb setup needs its buffers.
adapt("src/JSystem/JAudio2/JASAudioThread.cpp", "JSystem/JAudio2/JASAudioThread.cpp", [
    ("    pAudioThread->resume();\n}", "    pAudioThread->resume();\n    while (!sStarted) {\n        OSSleepTicks(OSMillisecondsToTicks(1));\n    }\n}", 1),
    ("    JASDriver::startDMA();\n", "    JASDriver::startDMA();\n    sStarted = true;\n", 1),
    ("void JASAudioThread::create(s32 threadPriority) {", "static volatile bool sStarted = false;\n\nvoid JASAudioThread::create(s32 threadPriority) {", 1),
])
# The music-event table and sequence index turn native as the game takes them (compat/smg/NativeResources.hpp).
adapt("src/Game/AudioLib/AudSystem.cpp", "Game/AudioLib/AudSystem.cpp", [
    ('#include "Game/AudioLib/AudSystem.hpp"\n', '#include "Game/AudioLib/AudSystem.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("    void* meSeq = pArchive->getResource(a2);\n",
     "    void* meSeq = pArchive->getResource(a2);\n    smgNativeMeTable(meTable, pArchive->getResSize(meTable));\n"
     "    smgNativeMeSequences(meSeq, pArchive->getResSize(meSeq));\n", 1),
    ("        mRemixMgr->setRemixSeqResource(pRemixArchive->getResource(static_cast< u16 >(0)));",
     "        void* remix = pRemixArchive->getResource(static_cast< u16 >(0));\n"
     "        smgNativeRemixSequence(remix, pRemixArchive->getResSize(remix));\n        mRemixMgr->setRemixSeqResource(remix);", 1),
])
# A chord table turns native as it is first taken, while its relocated flag (0 in either byte order) is still clear.
adapt("src/Game/RhythmLib/AudChordInfo.cpp", "Game/RhythmLib/AudChordInfo.cpp", [
    ("#include <JSystem/JKernel/JKRArchive.hpp>\n", "#include <JSystem/JKernel/JKRArchive.hpp>\n#include <smg/NativeResources.hpp>\n", 1),
    ("    bool loaded = setChordTableResource(pArchive->getResource((u16)id));",
     "    void* resource = pArchive->getResource((u16)id);\n    if (resource != nullptr && *(s32*)resource == 0) {\n"
     "        smgNativeChordTable(resource, pArchive->getResSize(resource));\n    }\n    bool loaded = setChordTableResource(resource);", 1),
])
# THP movies (docs/PORTING.md, "Movies"): SCGetSoundMode needs sc.h. The player copies the file's big-endian headers into its structs, and each frame
# starts with big-endian sizes; they turn native as each read lands. Audio records turn as they're decoded.
adapt("src/Game/Screen/THPSimplePlayerWrapper.cpp", "Game/Screen/THPSimplePlayerWrapper.cpp", [
    # The last preload's callback restarts this step, which read once more, into the first frame's buffer, after
    # which no callback turned it native; the Wii's slow drive mostly lost that race, the SD card never does.
    ("void THPSimplePlayerWrapper::exeReadPreLoad() {\n    if (MR::isFirstStep(this)) {\n",
     "void THPSimplePlayerWrapper::exeReadPreLoad() {\n    if (MR::isFirstStep(this) && _314) {\n", 1),
    ('#include <cstring>\n', '#include <cstring>\n#include <revolution/sc.h>\n#include <smg/NativeResources.hpp>\n\nextern "C" u32 smgThpFrameBytes;  // source/compat/Thp.cpp\n', 1),
    # The port keeps a copy of each frame to decode when drawn (source/compat/Thp.cpp), so it needs the frame's size.
    ("                    case 0:\n                        if (!videoDecode(ptr)) {",
     "                    case 0:\n                        smgThpFrameBytes = *compSize;\n                        if (!videoDecode(ptr)) {", 1),
    ("                case 0:\n                    if (!videoDecode(ptr)) {",
     "                case 0:\n                    smgThpFrameBytes = *compSize;\n                    if (!videoDecode(ptr)) {", 1),
    ("    memcpy(&mHeader, WorkBuffer, sizeof(mHeader));\n", "    memcpy(&mHeader, WorkBuffer, sizeof(mHeader));\n    smgNativeWords(&mHeader.version, 11);\n", 1),
    ("    memcpy(&mFrameComp, WorkBuffer, sizeof(mFrameComp));\n",
     "    memcpy(&mFrameComp, WorkBuffer, sizeof(mFrameComp));\n    smgNativeWords(&mFrameComp.numComponents, 1);\n", 1),
    ("    memcpy(&mVideoInfo, WorkBuffer, sizeof(mVideoInfo));\n", "    memcpy(&mVideoInfo, WorkBuffer, sizeof(mVideoInfo));\n    smgNativeWords(&mVideoInfo, 3);\n", 1),
    ("    memcpy(&mAudioInfo, WorkBuffer, sizeof(mAudioInfo));\n", "    memcpy(&mAudioInfo, WorkBuffer, sizeof(mAudioInfo));\n    smgNativeWords(&mAudioInfo, 4);\n", 1),
    ("    mCurOffset += mReadSize;\n    mReadSize = *(u32*)mReadBuffer[mReadIndex].ptr;\n",
     "    mCurOffset += mReadSize;\n    smgNativeWords(mReadBuffer[mReadIndex].ptr, 2 + mFrameComp.numComponents);\n"
     "    mReadSize = *(u32*)mReadBuffer[mReadIndex].ptr;\n", 1),
    ("    mCurOffset += mReadSize;\n    mReadSize = *(s32*)mReadBuffer[mReadIndex].ptr;\n",
     "    mCurOffset += mReadSize;\n    smgNativeWords(mReadBuffer[mReadIndex].ptr, 2 + mFrameComp.numComponents);\n"
     "    mReadSize = *(s32*)mReadBuffer[mReadIndex].ptr;\n", 1),
])
adapt("src/RVL_SDK/thp/THPAudio.c", "RVL_SDK/thp/THPAudio.c", [
    ('#include "revolution/thp.h"\n', '#include "revolution/thp.h"\n#include <smg/NativeResources.hpp>\n#include <cstring>\n', 1),
    ("    header = (THPAudioRecordHeader*)audioFrame;\n",
     "    THPAudioRecordHeader native;\n    memcpy(&native, audioFrame, sizeof native);\n    smgNativeWords(&native, 2);\n"
     "    smgNativeHalves(native.lCoef, (sizeof native - 8) / 2);\n    header = &native;\n", 1),
    # thp.h declares both extern; C let the definitions say static, C++ (which the port builds the SDK's C as) doesn't.
    ("static s32 __THPAudioGetNewSample(", "s32 __THPAudioGetNewSample(", 1),
    ("static void __THPAudioInitialize(", "void __THPAudioInitialize(", 1),
])
# THPGXYuv2RgbDraw is the port's (source/compat/Thp.cpp): it draws frames THPVideoDecode decoded into the port's planes.
adapt("src/Game/Screen/THPDraw.c", "Game/Screen/THPDraw.c", [],
      drop=["void THPGXYuv2RgbDraw(u8* y_data, u8* u_data, u8* v_data, s16 x, s16 y, s16 textureWidth, s16 textureHeight, s16 polygonWidth, s16 polygonHeight) {"],
      reason="the port decodes movie frames into its own planes")
# A failed allocation panics with an empty message on the Wii; the port's names the heap, size and alignment, after a
# census of what fills the heap (source/compat/GameHeap.cpp).
adapt("src/Game/System/HeapMemoryWatcher.cpp", "Game/System/HeapMemoryWatcher.cpp", [
    ('#include "Game/System/HeapMemoryWatcher.hpp"\n', '#include "Game/System/HeapMemoryWatcher.hpp"\n\nextern "C" void smgHeapCensus(void* heap);\n', 1),
    ("void HeapMemoryWatcher::memoryErrorCallback(void*, u32, int) {\n    OSPanic(__FILE__, 537, \"\");",
     "void HeapMemoryWatcher::memoryErrorCallback(void* pHeap, u32 size, int align) {\n"
     "    JKRHeap* heap = static_cast< JKRHeap* >(pHeap);\n"
     "    smgHeapCensus(pHeap);\n"
     "    OSPanic(__FILE__, 537, \"heap %p (%lu of %lu bytes free, %lu in one block) could not give %lu bytes aligned %d\", pHeap,\n"
     "            (unsigned long)heap->getTotalFreeSize(), (unsigned long)heap->mSize, (unsigned long)heap->getFreeSize(), (unsigned long)size, align);", 1),
])
# The Wii Remote speaker's table and waves turn native as they load, once each (SpkSystem::setResource).
# A race ghost's recording (GhostData*.arc's .gst, Cosmic Mario) is read a field at a time, its halves and words copied
# byte by byte in the disc's order: they are put together big-endian.
adapt("src/Game/Player/GhostPacket.cpp", "Game/Player/GhostPacket.cpp", [
    ("void GhostPacket::read(u32* pOut) {\n    read((u8*)pOut, 4);\n}",
     "void GhostPacket::read(u32* pOut) {\n    u8 bytes[4];\n    read(bytes, 4);\n"
     "    *pOut = (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 | bytes[3];\n}", 1),
    ("void GhostPacket::read(s16* pOut) {\n    read((u8*)pOut, 2);\n}",
     "void GhostPacket::read(s16* pOut) {\n    u8 bytes[2];\n    read(bytes, 2);\n    *pOut = (s16)(bytes[0] << 8 | bytes[1]);\n}", 1),
    ("    read((u8*)&pOut->x, 2);\n    read((u8*)&pOut->y, 2);\n    read((u8*)&pOut->z, 2);",
     "    read(&pOut->x);\n    read(&pOut->y);\n    read(&pOut->z);", 1),
])
adapt("src/Game/Speaker/SpkData.cpp", "Game/Speaker/SpkData.cpp", [
    ('#include "Game/Speaker/SpkData.hpp"\n', '#include "Game/Speaker/SpkData.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("    void* resource = mArchive->getResource(table);\n",
     "    void* resource = mArchive->getResource(table);\n    smgNativeSpeakerTable(resource, mArchive->getResSize(resource));\n", 1),
    ("    void* resource = mArchive->getResource(wave);\n",
     "    void* resource = mArchive->getResource(wave);\n    smgNativeSpeakerWaves(resource, mArchive->getResSize(resource));\n", 1),
])
# A sound's type is its item word's top byte, which the Wii's byte order put first in memory.
adapt("libs/JSystem/include/JSystem/JAudio2/JAUSoundTable.hpp", "include/JSystem/JAudio2/JAUSoundTable.hpp", [
    ("        return ((u8*)mItemOffsets)[index * 4];", "        return mItemOffsets[index] >> 24;", 1),
])
# The audio archive's parts turn native as it loads, before its interpreter reads them (smg::nativeAudioArchive);
# it arrives without its size, so the heap block holding it bounds it.
adapt("src/JSystem/JAudio2/JAUAudioArcLoader.cpp", "JSystem/JAudio2/JAUAudioArcLoader.cpp", [
    ('#include "JSystem/JKernel/JKRDvdRipper.hpp"\n', '#include "JSystem/JKernel/JKRDvdRipper.hpp"\n#include <smg/NativeResources.hpp>\n', 1),
    ("bool JAUAudioArcLoader::load(void const* pData) {\n    return parse(pData);",
     "bool JAUAudioArcLoader::load(void const* pData) {\n    smgNativeAudioArchive(const_cast< void* >(pData), JKRHeap::getSize(const_cast< void* >(pData), nullptr));\n"
     "    return parse(pData);", 1),
])
# The collection's group tables sit at odd offsets, which ARM's word loads can't take; they stay big-endian and are
# read by bytes (smg::nativeAudioArchive turns only the collection's header).
adapt("src/JSystem/JAudio2/JAUSeqCollection.cpp", "JSystem/JAudio2/JAUSeqCollection.cpp", [
    ("    u32 offset = mSoundTableOffsets[groupId];\n    JAUSeqCollectionSoundTable* pSoundTable = (JAUSeqCollectionSoundTable*)((u8*)mCollectionData + offset);\n\n"
     "    if (waveId >= pSoundTable->mWaveNum) {\n        return false;\n    }\n\n"
     "    pSeqData->set((void*)mCollectionData, pSoundTable->mWaveSeqOffsets[waveId]);",
     "    u32 offset = mSoundTableOffsets[groupId];\n    const u8* pSoundTable = (const u8*)mCollectionData + offset;\n"
     "    auto be32 = [](const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; };\n\n"
     "    if (waveId >= be32(pSoundTable)) {\n        return false;\n    }\n\n"
     "    pSeqData->set((void*)mCollectionData, be32(pSoundTable + 4 + 4 * waveId));", 1),
])
# A FIR filter's eight taps sit in the sequence, big-endian like the rest of it.
adapt("src/JSystem/JAudio2/JASSeqParser.cpp", "JSystem/JAudio2/JASSeqParser.cpp", [
    ("    track->setFIR((s16*)track->getSeqCtrl()->getAddr(args[0]));",
     "    const u8* taps = (const u8*)track->getSeqCtrl()->getAddr(args[0]);\n    s16 fir[8];\n    for (u32 i = 0; i < 8; i++) {\n"
     "        fir[i] = s16(taps[2 * i] << 8 | taps[2 * i + 1]);\n    }\n    track->setFIR(fir);", 1),
])
# The unit heap's table marks unit i as bit 0x80 >> (i & 7) of byte i / 8 (Overwrite.cpp's do_alloc and do_free), and
# its searches read four bytes at once, testing 0x80000000 >> bit: the Wii's byte order, so they read it big-endian.
adapt("src/JSystem/JKernel/JKRUnitHeap.cpp", "JSystem/JKernel/JKRUnitHeap.cpp", [
    ("static const u8 bitTable[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};\n",
     "static const u8 bitTable[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};\n\n"
     "static inline u32 batWord(const u32* word) {\n    const u8* bytes = reinterpret_cast< const u8* >(word);\n"
     "    return u32(bytes[0]) << 24 | u32(bytes[1]) << 16 | u32(bytes[2]) << 8 | bytes[3];\n}\n", 1),
    ("            word = *words;\n", "            word = batWord(words);\n", 2),
    ("        u32 word = words != end ? *words : 0xFFFFFFFF;\n", "        u32 word = words != end ? batWord(words) : 0xFFFFFFFF;\n", 2),
])
# The layout's name points into a buffer declared inside the if and is used after it ends; Metrowerks kept that
# buffer's stack slot, GCC reuses it for archiveName, so the buffers live for the whole constructor.
adapt("src/Game/Screen/LayoutManager.cpp", "Game/Screen/LayoutManager.cpp", [
    ("    if (useArchiveNamePrefix) {\n        char fileNameWithoutExtension[0x60];\n        char fileNameFromPrefix[0x80];\n",
     "    char fileNameWithoutExtension[0x60];\n    char fileNameFromPrefix[0x80];\n    if (useArchiveNamePrefix) {\n", 1),
    # The 5:3 screen's own share of a layout's width (compat/smg/Screen.hpp), for the panes that adjust to the screen.
    ('#include "Game/Screen/LayoutManager.hpp"\n', '#include "Game/Screen/LayoutManager.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("        scale.x = 0.75f;", "        scale.x = smg::screen::LayoutAdjustX;", 1),
])
# The Wii's HOME Button Menu is PowerPC code loaded from the disc; the 3DS's HOME button replaces it.
adapt("src/Game/System/HomeButtonMenuWrapper.cpp", "Game/System/HomeButtonMenuWrapper.cpp", [], drop=[
    "void RSO::setupRsoHomeButtonMenu() {", "void RSO::HBMCreate(const HBMDataInfo* pHBInfo) {", "void RSO::HBMInit() {",
    "void RSO::HBMCalc(const HBMControllerData* pController) {", "void RSO::HBMDraw() {", "HBMSelectBtnNum RSO::HBMGetSelectBtnNum() {",
    "void RSO::HBMSetAdjustFlag(int flag) {", "void RSO::HBMStartBlackOut() {"],
      reason="the port's (source/compat/HomeButtonMenu.cpp)")
# Metrowerks sized every enum as an int (-enum int); devkitARM's are one byte. These sit in what J3D reads straight from
# model files (GXVtxAttrFmtList, GXVtxDescList, MAT3's cull modes), so each gets an int-sized value to keep its size.
adapt("libs/RVL_SDK/include/revolution/gx/GXEnum.h", "include/revolution/gx/GXEnum.h", [
    ("  GX_VA_NULL = 0xff\n} GXAttr;", "  GX_VA_NULL = 0xff,\n  GX_ATTR_INT_SIZED = 0x7FFFFFFF\n} GXAttr;", 1),
    ("  GX_INDEX16\n\n} GXAttrType;", "  GX_INDEX16,\n  GX_ATTR_TYPE_INT_SIZED = 0x7FFFFFFF\n} GXAttrType;", 1),
    ("  GX_RGBA8 = 5\n} GXCompType;", "  GX_RGBA8 = 5,\n  GX_COMP_TYPE_INT_SIZED = 0x7FFFFFFF\n} GXCompType;", 1),
    ("  GX_TEX_ST = 1\n} GXCompCnt;", "  GX_TEX_ST = 1,\n  GX_COMP_CNT_INT_SIZED = 0x7FFFFFFF\n} GXCompCnt;", 1),
    ("  GX_CULL_ALL\n} GXCullMode;", "  GX_CULL_ALL,\n  GX_CULL_MODE_INT_SIZED = 0x7FFFFFFF\n} GXCullMode;", 1),
])
# MSL's own wide-string functions, built as C over newlib's headers (compat/smg/MslWide.h stands in for MSL's).
adapt("src/MSL_C/wstring.c", "MSL_C/wstring.c", [('#include "size_t.h"\n#include "wchar_t.h"\n', "#include <smg/MslWide.h>\n", 1)])
adapt("src/MSL_C/wmem.c", "MSL_C/wmem.c", [('#include "wchar_io.h"\n#include "cstring"\n', "#include <smg/MslWide.h>\n", 1)])
# The game never calls setlocale: MSL's "C" locale decodes and encodes a character as one byte.
adapt("src/MSL_C/mbstring.c", "MSL_C/mbstring.c", [
    ('#include "mbstring.h"\n#include "locale.h"\n#include "cstring"\n#pragma exceptions on\n', "#include <smg/MslWide.h>\n", 1),
    ("    return _current_locale.ctype_cmpt_ptr->decode_mb(pDest, s, num);", "    return __mbtowc_noconv(pDest, s, num);", 1),
    ("inline int wctomb(char *pSrc, wchar_t wchar) {\n    return _current_locale.ctype_cmpt_ptr->encode_wc(pSrc, wchar);",
     "int wctomb(char *pSrc, wchar_t wchar) {\n    return __wctomb_noconv(pSrc, wchar);", 1),
])
# PowerPC's va_list is an array, so the argument itself was the pointer parse_format advances; ARM's is a struct.
adapt("src/MSL_C/wprintf.c", "MSL_C/wprintf.c", [
    ('#include "ansi_fp.h"\n#include "stdio_api.h"\n#include "va_list.h"\n#include "wmem.h"\n#include "wstring.h"\n#include "cmath"\n'
     '#include "cstdarg"\n#include "cstdlib"\n#include "cwctype"\n#include "wchar_t.h"\n\n#pragma exceptions on\n', "#include <smg/MslWide.h>\n", 1),
    ("parse_format(format_ptr, (va_list*)arg, &format);", "parse_format(format_ptr, &arg, &format);", 1),
])
# Colors packed into a u32 through memory read 0xRRGGBBAA on the Wii and byte-reversed on ARM; these pack and
# unpack the value the Wii's way, so literals like 0xFF0000FF and u32s bound for the GPU keep their meaning.
adapt("libs/nw4r/include/nw4r/ut/Color.h", "include/nw4r/ut/Color.h", [
    ("                ToU32ref() = color;\n                return *this;",
     "                r = static_cast< u8 >(color >> 24);\n                g = static_cast< u8 >(color >> 16);\n"
     "                b = static_cast< u8 >(color >> 8);\n                a = static_cast< u8 >(color);\n                return *this;", 1),
    ("                return operator=(*reinterpret_cast< const u32* >(&color));",
     "                *static_cast< GXColor* >(this) = color;\n                return *this;", 1),
    ("            operator u32() const {\n                return ToU32ref();\n            }",
     "            operator u32() const {\n                return u32(r) << 24 | u32(g) << 16 | u32(b) << 8 | a;\n            }", 1),
])
adapt("include/Game/Util/Color.hpp", "include/Game/Util/Color.hpp", [
    ("    Color8(u32 color) {\n        mColor = color;\n    }",
     "    Color8(u32 color) {\n        set(u8(color >> 24), u8(color >> 16), u8(color >> 8), u8(color));\n    }", 1),
    ("    inline operator u32() const {\n        return mColor;\n    }",
     "    inline operator u32() const {\n        return u32(r) << 24 | u32(g) << 16 | u32(b) << 8 | a;\n    }", 1),
])
# Each sound's flags are a word whose top byte the Wii read as the union's first byte: how it plays (a one-shot, held
# or system sound) and its underwater variant.
adapt("src/Game/Player/MarioSound.cpp", "Game/Player/MarioSound.cpp", [
    ("        switch (soundlist[index]._8._4[0] & 0x3) {", "        switch ((soundlist[index]._8._0 >> 24) & 0x3) {", 2),
    ("        switch (soundlist[index]._8._4[0] & ~0x3) {", "        switch ((soundlist[index]._8._0 >> 24) & ~0x3) {", 1),
])
# A rush's end info sets its damage kind by bitfield and Mario's rush reads it from the word (bits 24-27, where
# Metrowerks put it); GCC on a little-endian target fills from the bottom, so the declarations swap places there.
adapt("include/Game/Player/RushEndInfo.hpp", "include/Game/Player/RushEndInfo.hpp", [
    ("            u32 _0 : 4;\n            u32 mDamageType : 4;\n            u32 _8 : 24;\n",
     "#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n            u32 _8 : 24;\n            u32 mDamageType : 4;\n            u32 _0 : 4;\n#else\n"
     "            u32 _0 : 4;\n            u32 mDamageType : 4;\n            u32 _8 : 24;\n#endif\n", 1),
])
# The effect tables' flag words are written as words and read by byte, the Wii's first byte being the top one.
adapt("src/Game/Player/MarioEffect.cpp", "Game/Player/MarioEffect.cpp", [
    ("            u8 mByte0;\n            u8 mByte1;\n            u8 mByte2;\n            u8 mByte3;\n",
     "            u8 mByte3;\n            u8 mByte2;\n            u8 mByte1;\n            u8 mByte0;\n", 2),
])
# A spiral camera's start and end times are the halves of a BCSV word, the Wii's first half being the top one.
adapt("src/Game/Camera/CamTranslatorSpiral.cpp", "Game/Camera/CamTranslatorSpiral.cpp", [
    ("    s32 startTime = reinterpret_cast< s16* >(&general->mNum1)[0];\n    s32 endTime = reinterpret_cast< s16* >(&general->mNum1)[1];",
     "    s32 startTime = s16(general->mNum1 >> 16);\n    s32 endTime = s16(general->mNum1);", 1),
])
# On the Wii, bit 0 of J3DModelX's flag word (display list 0) is Metrowerks' last field: _1F.
adapt("src/Game/Player/MarioAnimator.cpp", "Game/Player/MarioAnimator.cpp", [
    ("        u32* modelFlags = (u32*)&model->mFlags;\n        *modelFlags |= 1;", "        model->mFlags._1F = true;", 1),
    ("        u32* modelFlags = (u32*)&model->mFlags;\n        *modelFlags &= ~1;", "        model->mFlags._1F = false;", 1),
])
# The draw-state bits these constants set as Metrowerks packs bitfields (from the top): fields 0xB and 0x12.
adapt("src/Game/Player/MarioJump.cpp", "Game/Player/MarioJump.cpp", [
    ("        _1C_WORD |= 0x00100000;", "        _1C._B = true;", 1),
    ("    _1C_WORD |= 0x2000;", "    _1C.mIsUnderwater = true;", 1),
])
# A Yaz0 header's words are big-endian; the decoder's end is the decompressed size JKRDecompExpandSize reads.
adapt("src/JSystem/JKernel/JKRDecomp.cpp", "JSystem/JKernel/JKRDecomp.cpp", [
    ("    u32 decompSize = ((s32)pDst + *(u32*)(pSrc + 4)) - a4;", "    u32 decompSize = ((s32)pDst + JKRDecompExpandSize(pSrc)) - a4;", 1),
    ("    if (a4 > *(u32*)pSrc) {", "    if (a4 > (u32(pSrc[0]) << 24 | u32(pSrc[1]) << 16 | u32(pSrc[2]) << 8 | pSrc[3])) {", 1),
])
# A primitive's vertex count in a display list is the GPU's big-endian half, as the model file holds it.
adapt("src/JSystem/J3DGraphBase/J3DShapeDraw.cpp", "JSystem/J3DGraphBase/J3DShapeDraw.cpp", [
    ("        int vtxNum = *reinterpret_cast< u16* >(dl);", "        int vtxNum = dl[0] << 8 | dl[1];", 1),
    ("        int vtxNum = *reinterpret_cast< u16* >(oldDL);", "        int vtxNum = oldDL[0] << 8 | oldDL[1];", 1),
    ("        *reinterpret_cast< u16* >(newDL) = vtxNum;", "        newDL[0] = static_cast< u8 >(vtxNum >> 8);\n        newDL[1] = static_cast< u8 >(vtxNum);", 1),
])
# A TEV stage's two BP commands are its bytes as the Wii's word load read them: the register on top.
adapt("libs/JSystem/include/JSystem/J3DGraphBase/J3DTevs.hpp", "include/JSystem/J3DGraphBase/J3DTevs.hpp", [
    ("        J3DGDWriteBPCmd(*(u32*)&field_0x0);\n        J3DGDWriteBPCmd(*(u32*)&field_0x4);",
     "        J3DGDWriteBPCmd(u32(field_0x0) << 24 | u32(mTevColorOp) << 16 | u32(mTevColorAB) << 8 | mTevColorCD);\n"
     "        J3DGDWriteBPCmd(u32(field_0x4) << 24 | u32(mTevAlphaOp) << 16 | u32(mTevAlphaAB) << 8 | mTevSwapModeInfo);", 1),
])
adapt("libs/JSystem/include/JSystem/JUtility/TColor.hpp", "include/JSystem/JUtility/TColor.hpp", [
    ("            *reinterpret_cast< u32* >(&r) = u32Color;",
     "            set(u8(u32Color >> 24), u8(u32Color >> 16), u8(u32Color >> 8), u8(u32Color));", 1),
    ("            return *reinterpret_cast< const u32* >(&r);", "            return u32(r) << 24 | u32(g) << 16 | u32(b) << 8 | a;", 1),
])
# Reads past an array, which gcc treats as unreachable and may fold the surrounding loop or copy away.
# Nintendo's loop runs past three horizontal tasks into mInfinityTasks, which createInfinityTask then replaces;
# stopping at three leaves the same tasks, less four leaked allocations.
adapt("src/Game/Camera/CameraShaker.cpp", "Game/Camera/CameraShaker.cpp", [
    ("    // Should be i < NR_HORIZONTAL_TASKS\n    for (u32 i = 0; i < NR_VERTICAL_TASKS; i++) {",
     "    for (u32 i = 0; i < NR_HORIZONTAL_TASKS; i++) {", 1),
])
# Petari's J3DMatBlock.cpp doesn't match yet; the block has four texture numbers, as patchTexNo above reads them.
adapt("src/JSystem/J3DGraphBase/J3DMatBlock.cpp", "JSystem/J3DGraphBase/J3DMatBlock.cpp", [
    ("    u32 tevStageNum = mTevStageNum;\n\n    for (u32 i = 0; i < 8; i++) {\n        if (mTexNo[i] != 0xffff) {",
     "    u32 tevStageNum = mTevStageNum;\n\n    for (u32 i = 0; i < ARRAY_SIZE(mTexNo); i++) {\n        if (mTexNo[i] != 0xffff) {", 1),
    # Material and ambient colors go to the GPU as the word the Wii's load of their bytes gave: R on top.
    ("#include <cstdio>\n", "#include <cstdio>\n\nstatic inline u32 smgColorWord(const GXColor& c) { return u32(c.r) << 24 | u32(c.g) << 16 | u32(c.b) << 8 | c.a; }\n", 1),
    ("    J3DGDWrite_u32(*(u32*)color);\n    J3DGDWrite_u32(*(u32*)(color + 1));",
     "    J3DGDWrite_u32(smgColorWord(color[0]));\n    J3DGDWrite_u32(smgColorWord(color[1]));", 2),
])
adapt("src/Game/MapObj/Swinger.cpp", "Game/MapObj/Swinger.cpp", [
    ("_3C((*a2)[0], (*a2)[4], (*a2)[8]),\n      _48((*a2)[1], (*a2)[5], (*a2)[9]), _54((*a2)[2], (*a2)[6], (*a2)[10])",
     "_3C(a2[0][0], a2[1][0], a2[2][0]),\n      _48(a2[0][1], a2[1][1], a2[2][1]), _54(a2[0][2], a2[1][2], a2[2][2])", 1),
])
# The projection copies sixteen floats from the 3x4 table; on the Wii the last four came from the next object in
# .data (a vtable and a string), which a zero row stands in for.
adapt("src/Game/Screen/StarPointerDirector.cpp", "Game/Screen/StarPointerDirector.cpp", [
    ("    static f32 mtx_identity[3][4] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};",
     "    static f32 mtx_identity[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, {}};", 1),
])

# Last: everything above may adapt a file these build on.
# The rest of each file the scene libraries take extracts from, for the whole-game library: the same copy the
# build would use otherwise, less what the extracts already define.
for relative, firsts in extracted.items():
    target, text = written.get(relative, (relative[len("src/"):], (root / relative).read_text()))
    # A copy that already leaves them out (LiveActor.cpp, RailRider.cpp) is the whole file its library builds.
    lines = text.splitlines()

    def kept_here(first):
        # Inline and template definitions stay: the rest of the file uses them, and copies of those link fine.
        at = next((i for i, line in enumerate(lines) if first in line and "// Left out" not in line), None)
        return at is None or first.startswith("inline ") or (at > 0 and lines[at - 1].startswith("template"))
    present = [first for first in firsts if not kept_here(first)]
    for before, after in extracted_fixes.get(relative, []):
        if before not in text:
            raise SystemExit(f"Upstream changed; review {relative}: {before!r}")
        text = text.replace(before, after)
    text = drop_definitions(text, relative, present, "built from an extract of this file")
    if relative in NATIVE and relative not in written:
        text = drop_definitions(text, relative, *reversed(NATIVE[relative]))
    text = "// Generated Petari adaptation; see tools/prepare-petari.py.\n" + text.removeprefix("// Generated Petari adaptation; see tools/prepare-petari.py.\n")
    destination = output / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    emit(destination, text)
for relative in NATIVE:
    if relative not in written and relative not in extracted:
        adapt(relative, relative[len("src/"):], [])
# Archives: the file entry's flag byte is the high byte of its word. Metrowerks fills bitfields from the top,
# GCC for a little-endian target from the bottom, so the declarations swap places there.
adapt("libs/JSystem/include/JSystem/JKernel/JKRArchive.hpp", "include/JSystem/JKernel/JKRArchive.hpp", [
    ("        /* 0x4 */ u32 mFlag : 8;\n        /* 0x5 */ u32 mNameOffset : 24;",
     "#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__\n        u32 mNameOffset : 24;\n        u32 mFlag : 8;\n#else\n"
     "        /* 0x4 */ u32 mFlag : 8;\n        /* 0x5 */ u32 mNameOffset : 24;\n#endif", 1),
])
# A memory archive's tables turn native as it takes the archive (compat/smg/NativeResources.hpp).
adapt("src/JSystem/JKernel/JKRMemArchive.cpp", "JSystem/JKernel/JKRMemArchive.cpp", [
    ('#include "JSystem/JKernel/JKRMemArchive.hpp"', '#include "JSystem/JKernel/JKRMemArchive.hpp"\n#include <smg/NativeResources.hpp>', 1),
    ("    if (mHeader == nullptr) {\n        mMountMode = MOUNT_MODE_0;\n    } else {\n        mInfoBlock = reinterpret_cast< RarcInfoBlock* >(reinterpret_cast< u8* >(mHeader) + mHeader->mHeaderSize);",
     "    if (mHeader == nullptr) {\n        OSReport(\"JKRMemArchive: disc entry %ld did not load\\n\", (long)entryNum);\n        mMountMode = MOUNT_MODE_0;\n"
     "    } else {\n        smgNativeRarc(mHeader);\n        mInfoBlock = reinterpret_cast< RarcInfoBlock* >(reinterpret_cast< u8* >(mHeader) + mHeader->mHeaderSize);", 1),
    ("    mHeader = reinterpret_cast< RarcHeader* >(pData);\n    mInfoBlock =",
     "    mHeader = reinterpret_cast< RarcHeader* >(pData);\n    smgNativeRarc(mHeader);\n    mInfoBlock =", 1),
], cases=["    case JKR_COMPRESSION_SZS:"])

# The SDK's own GX (petari_game): its paired-single writers get C bodies. Every one writes the write-gather pipe,
# whatever address it is handed, so each value goes through the GX FIFO macros in order.
gx_transform = (root / "src/RVL_SDK/gx/GXTransform.c").read_text()
gx_writers = gx_transform[gx_transform.index("static void WriteProjPS("):gx_transform.index("void __GXSetProjection(void) {")]
gx_writers_c = """#ifdef __MWERKS__
""" + gx_writers + """#else
static void WriteProjPS(const f32 proj[6], volatile void*) {
    for (int i = 0; i < 6; ++i) GX_WRITE_F32(proj[i]);
}

static void Copy6Floats(const f32 src[6], f32 dst[6]) {
    for (int i = 0; i < 6; ++i) dst[i] = src[i];
}
#endif

"""
gx_matrices = gx_transform[gx_transform.index("static void  WriteMTXPS3x3from3x4("):gx_transform.index("void GXLoadPosMtxImm(")]
gx_matrices_c = """#ifdef __MWERKS__
""" + gx_matrices + """#else
static void WriteMTXPS3x3from3x4(void* mtx, volatile void*) {
    const f32(*m)[4] = (const f32(*)[4])mtx;
    for (int row = 0; row < 3; ++row) smgGXFifoFloats(m[row], 3);
}

static void WriteMTXPS4x3(const f32 src[3][4], volatile void*) { smgGXFifoFloats(&src[0][0], 12); }

static void WriteMTXPS4x2(const f32 mtx[3][4], volatile void*) { smgGXFifoFloats(&mtx[0][0], 8); }
#endif

"""
# GXColor and GXColorS10 arguments were read through u32 casts, giving their bytes in the Wii's order only there.
inc = ('#include "revolution/gx/GXRegs.h"\n', '#include "revolution/gx/GXRegs.h"\n#include <smg/GxPrivate.h>\n', 1)
adapt("src/RVL_SDK/gx/GXTev.c", "RVL_SDK/gx/GXTev.c", [
    ('#include "revolution/gx/GXTev.h"\n', '#include "revolution/gx/GXTev.h"\n#include <smg/GxPrivate.h>\n', 1),
    ("    rgba = *(u32*)(&color);", "    rgba = smgColorWord(color);", 2),
    ("    sRG = *(u32*)(&color);\n    sBA = *((u32*)(&color) + 1);",
     "    sRG = (u32)(u16)color.r << 16 | (u16)color.g;\n    sBA = (u32)(u16)color.b << 16 | (u16)color.a;", 1),
    # GXSetTevOp's presets are bitfield structs read as register words; GCC packs bitfields from the other end.
    ("    tevReg = ((*ctmp & ~0xFF000000) | (tevReg & 0xFF000000));",
     "    tevReg = ((smgTevColorWord((const tev_color_env_t*)ctmp) & ~0xFF000000) | (tevReg & 0xFF000000));", 1),
    ("    tevReg = ((*atmp & ~0xFF00000F) | (tevReg & 0xFF00000F));",
     "    tevReg = ((smgTevAlphaWord((const tev_alpha_env_t*)atmp) & ~0xFF00000F) | (tevReg & 0xFF00000F));", 1),
    # A TEV color's BG half goes three times for the Wii GPU's register latch; the port's processor takes it once (a
    # particle sets two colors, so this was four of its loads).
    ("    GX_WRITE_RA_REG(regBG);\n    GX_WRITE_RA_REG(regBG);\n    GX_WRITE_RA_REG(regBG);\n", "    GX_WRITE_RA_REG(regBG);\n", 2),
    # The two halves of a color register, and of the Z texture's, go in with one look for room.
    ("    GX_WRITE_RA_REG(regRA);\n    GX_WRITE_RA_REG(regBG);\n", "    GX_WRITE_RA_REG2(regRA, regBG);\n", 2),
    ("    GX_WRITE_RA_REG(regRA);\n    GX_WRITE_RA_REG(regBG); \n", "    GX_WRITE_RA_REG2(regRA, regBG);\n", 1),
    ("    GX_WRITE_RA_REG(zenv0);\n    GX_WRITE_RA_REG(zenv1);\n", "    GX_WRITE_RA_REG2(zenv0, zenv1);\n", 1),
])
# A texture's six registers go in with one look for room.
adapt("src/RVL_SDK/gx/GXTexture.c", "RVL_SDK/gx/GXTexture.c", [
    ("    GX_WRITE_RA_REG(t->mode0);\n    GX_WRITE_RA_REG(t->mode1);\n    GX_WRITE_RA_REG(t->image0);\n"
     "    GX_WRITE_RA_REG(r->image1);\n    GX_WRITE_RA_REG(r->image2);\n    GX_WRITE_RA_REG(t->image3);\n",
     "    smgGXFifoRun(6 * 5, [&](auto& out) {\n        out.bp(t->mode0), out.bp(t->mode1), out.bp(t->image0);\n"
     "        out.bp(r->image1), out.bp(r->image2), out.bp(t->image3);\n    });\n", 1),
])
adapt("src/RVL_SDK/gx/GXPixel.c", "RVL_SDK/gx/GXPixel.c", [inc, ("    rgba = *(u32*)(&color);", "    rgba = smgColorWord(color);", 1)])
adapt("src/RVL_SDK/gx/GXTransform.c", "RVL_SDK/gx/GXTransform.c", [
    (gx_writers, gx_writers_c, 1), (gx_matrices, gx_matrices_c, 1), inc,
    ("    GX_WRITE_U8(CP_OPCODE(0, CP_CMD_XF_IDXLDREG_C));\n    GX_WRITE_U32(reg);",
     "    smgGXLoadIndexedNow(CP_OPCODE(0, CP_CMD_XF_IDXLDREG_C), reg >> 16, reg & 0xFFFF);", 1),
    # A matrix sent whole (every particle sends one): the same bytes, with one look for room.
    ("    GX_WRITE_U8(CP_OPCODE(0, CP_CMD_XF_LOADREGS));\n    GX_WRITE_U32(reg);\n    WriteMTXPS4x3(mtx, (volatile void*)GX_FIFO_ADDR);\n",
     "    smgGXFifoXfLoad(reg, &mtx[0][0], 12);\n", 1),
    ("    GX_WRITE_U8(CP_OPCODE(0, CP_CMD_XF_LOADREGS));\n    GX_WRITE_U32(reg);\n\n    WriteMTXPS3x3from3x4((void*)mtx, (volatile void*)0xCC008000);\n",
     "    smgGXFifoXfLoadNrm(reg, &mtx[0][0]);\n", 1),
    ("    GX_WRITE_U8(CP_OPCODE(0, CP_CMD_XF_LOADREGS));\n    GX_WRITE_U32(reg);\n\n    if (type == GX_MTX3x4) {\n"
     "        WriteMTXPS4x3(mtx, (volatile void*)GX_FIFO_ADDR);\n    } else {\n        WriteMTXPS4x2(mtx, (volatile void*)GX_FIFO_ADDR);\n    }\n",
     "    smgGXFifoXfLoad(reg, &mtx[0][0], count);\n", 1),
])
# GXSetArray's matrix and light arrays are where an indexed load reads (smgGXLoadIndexedNow).
adapt("src/RVL_SDK/gx/GXAttr.c", "RVL_SDK/gx/GXAttr.c", [
    ('#include "revolution/gx/GXRegs.h"\n', '#include "revolution/gx/GXRegs.h"\n#include <smg/GxPrivate.h>\n', 1),
    ("    GX_WRITE_CP_STRM_REG(CP_ARRAY_STRIDE, cpAttr, (u32)stride);\n",
     "    GX_WRITE_CP_STRM_REG(CP_ARRAY_STRIDE, cpAttr, (u32)stride);\n    if (cpAttr >= 12) smgGXMatrixArray(cpAttr - 12, base_ptr, stride);\n", 1),
])
adapt("src/RVL_SDK/gx/GXLight.c", "RVL_SDK/gx/GXLight.c", [
    inc,
    ("    obj->Color = *(u32*)(&color);", "    obj->Color = smgColorWord(color);", 1),
    ("(*(u32*)(&amb_color)) >> 8", "smgColorWord(amb_color) >> 8", 2), ("*(u32*)(&amb_color))", "smgColorWord(amb_color))", 2),
    ("(*(u32*)(&mat_color)) >> 8", "smgColorWord(mat_color) >> 8", 2), ("*(u32*)(&mat_color))", "smgColorWord(mat_color))", 2),
    ("#else\nstatic inline void WriteLightObjPS(const GXLightObjInt* lt_obj, void* dest);\n#endif",
     "#else\n// Three reserved words, the color, then the attenuation, position and direction, as the asm stores them.\n"
     "template <class Out> static inline void WriteLightObjPS(Out& out, const GXLightObjInt* lt_obj) {\n"
     "    for (int i = 0; i < 3; ++i) out.u32(0);\n    out.u32(lt_obj->Color);\n"
     "    for (int i = 0; i < 3; ++i) out.f32(lt_obj->a[i]);\n    for (int i = 0; i < 3; ++i) out.f32(lt_obj->k[i]);\n"
     "    for (int i = 0; i < 3; ++i) out.f32(lt_obj->lpos[i]);\n    for (int i = 0; i < 3; ++i) out.f32(lt_obj->ldir[i]);\n}\n#endif", 1),
    # The whole load, its header and sixteen words, goes in with one look for room.
    ("    GX_WRITE_U8(CP_OPCODE(0, 2));\n    GX_WRITE_U32(CP_XF_LOADREGS((addr), 0xF));\n    WriteLightObjPS(obj, (void*)0xCC008000);\n",
     "#ifdef __MWERKS__\n    GX_WRITE_U8(CP_OPCODE(0, 2));\n    GX_WRITE_U32(CP_XF_LOADREGS((addr), 0xF));\n    WriteLightObjPS(obj, (void*)0xCC008000);\n#else\n"
     "    smgGXFifoRun(5 + 4 * 16, [&](auto& out) {\n        out.u8(CP_OPCODE(0, 2));\n        out.u32(CP_XF_LOADREGS((addr), 0xF));\n"
     "        WriteLightObjPS(out, obj);\n    });\n#endif\n", 1),
])
adapt("src/RVL_SDK/gx/GXInit.c", "RVL_SDK/gx/GXInit.c", [
    ('#include "revolution/gx.h"\n', '#include "revolution/gx.h"\n#include <smg/GxPrivate.h>\n', 1),
    # The GPU's register blocks are the port's memory (source/compat/GxFifo.cpp), not the Wii's uncached addresses.
    ("    __piReg = (void*)OSPhysicalToUncached(0x0c003000);\n    __cpReg = (void*)OSPhysicalToUncached(0x0c000000);\n"
     "    __peReg = (void*)OSPhysicalToUncached(0x0c001000);\n    __memReg = (void*)OSPhysicalToUncached(0x0c004000);",
     "    __piReg = smgGXRegisters(0);\n    __cpReg = smgGXRegisters(1);\n    __peReg = smgGXRegisters(2);\n    __memReg = smgGXRegisters(3);", 1),
])
adapt("src/RVL_SDK/gx/GXFrameBuf.c", "RVL_SDK/gx/GXFrameBuf.c", [
    ('#include "revolution/gx/GXRegs.h"\n', '#include "revolution/gx/GXRegs.h"\n#include <smg/GxPrivate.h>\n', 1),
    ("    GX_WRITE_RA_REG(gx->cpDisp);\n", "    GX_WRITE_RA_REG(gx->cpDisp);\n    smgDisplayCopyIssued();\n", 1),
])
adapt("src/RVL_SDK/gx/GXDisplayList.c", "RVL_SDK/gx/GXDisplayList.c", [
    ('#include "revolution/gx.h"\n', '#include "revolution/gx.h"\n#include <smg/GxPrivate.h>\n', 1),
    ("    GX_WRITE_U8(0x40);\n    GX_WRITE_U32((u32)list);\n    GX_WRITE_U32((u32)nbytes);\n}\n",
     "    smgGXFifoPut3(0x40, 1, (u32)list, 4, (u32)nbytes, 4);\n}\n\n"
     "void smgGXCallDisplayListNow(const void* list, u32 nbytes) {\n    if (gx->dirtyState) {\n        __GXSetDirtyState();\n    }\n\n"
     "    if (!(*(u32*)(&gx->vNumNot))) {\n        __GXSendFlushPrim();\n    }\n\n    smgGXFifoBytes(list, nbytes);\n}\n", 1),
])
# The flush primitive is a strip of one repeated vertex, for the Wii GPU's vertex cache after matrix loads: it covers no
# pixels and the port's GPU path has no such cache, so none is sent (JPA would send one before every particle).
adapt("src/RVL_SDK/gx/GXGeometry.c", "RVL_SDK/gx/GXGeometry.c", [
    ("    GX_WRITE_U8((vtxfmt | type));\n    GX_WRITE_U16(nverts);\n}", "    smgGXFifoPut2(vtxfmt | type, 1, nverts, 2);\n}", 1),
    ("""void __GXSendFlushPrim(void) {
    u32 i, numD;
    numD = (u32)(gx->vNum * gx->vLim);
    GX_WRITE_U8((GX_VTXFMT0 | GX_TRIANGLESTRIP));
    GX_WRITE_U16(gx->vNum);

    for (i = 0; i < numD; i += 4) {
        GX_WRITE_U32(0);
    }

    gx->bpSentNot = GX_TRUE;
}""", """void __GXSendFlushPrim(void) {
    gx->bpSentNot = GX_TRUE;
}""", 1),
])
# GXPeekARGB and GXPeekZ read the EFB through the Wii's memory window onto it; reading the port's render target back
# is still to come, so each stops by name.
# GXFlush pushes the write-gather pipe out to the GPU: the port hands the thread's write buffer to its command processor.
adapt("src/RVL_SDK/gx/GXMisc.c", "RVL_SDK/gx/GXMisc.c", [
    ('#include "revolution/gx.h"\n', '#include "revolution/gx.h"\n#include <smg/GxPrivate.h>\n', 1),
    ("    GX_WRITE_U32(0);\n    PPCSync();\n}", "    GX_WRITE_U32(0);\n    smgGXFifoFlush();\n    PPCSync();\n}", 1),
    # The EFB's pixels come from the port's renderer, which reads its target back.
    ("void GXPeekARGB(u16 x, u16 y, u32* color) {\n    u32 addr = (u32) OSPhysicalToUncached(0x8000000);\n    SC_PE_PI_EFB_ADDR_SET_X(addr, x);\n"
     "    SC_PE_PI_EFB_ADDR_SET_Y(addr, y);\n    SC_PE_PI_EFB_ADDR_SET_TYPE(addr,  0);\n    *color = (*(u32 *)addr);\n}",
     "void GXPeekARGB(u16 x, u16 y, u32* color) {\n    *color = smgPeekEfb(x, y, 0);\n}", 1),
    ("void GXPeekZ(u16 x, u16 y, u32* z)  {\n    u32 addr = (u32) OSPhysicalToUncached(0x8000000);\n    SC_PE_PI_EFB_ADDR_SET_X(addr, x);\n"
     "    SC_PE_PI_EFB_ADDR_SET_Y(addr, y);\n    SC_PE_PI_EFB_ADDR_SET_TYPE(addr,  1);\n    *z = *(u32*)addr;\n}",
     "void GXPeekZ(u16 x, u16 y, u32* z)  {\n    *z = smgPeekEfb(x, y, 1);\n}", 1),
    # Draw syncs set are counted, for the wait before a scene is destroyed (smgWaitDrawSyncs).
    ("    en = OSDisableInterrupts();\n    reg = PE_TOKEN_INT(token, 0x48);", "    en = OSDisableInterrupts();\n    smgDrawSyncIssued();\n    reg = PE_TOKEN_INT(token, 0x48);", 1),
])

# The Wii's hardware registers: the GX library's processor interface and memory interface blocks are the port's
# register memory (source/compat/GxFifo.cpp), the same blocks __piReg and __memReg view; every other block has no
# counterpart here, so touching one stops the program by name.
adapt("libs/RVL_SDK/include/private/flipper.h", "include/private/flipper.h", [
    ("#else\nvu16 __VIRegs[0x3B];\nvu32 __PIRegs[0xC];\nvu16 __DSPRegs[0x20];\nvu32 __AIRegs[0x8];\nvu32 __EXIRegs[0x10];\n"
     "vu16 __MEMRegs[0x40];\nvu32 __DIRegs[16];\nvu32 __SIRegs[64];\nvu32 __ACRRegs[89];\nvu32 __IPCRegs[4];\n#endif",
     "#else\n#include <smg/GxPrivate.h>\n#define __PIRegs ((vu32*)smgGXRegisters(0))\n#define __MEMRegs ((vu16*)smgGXRegisters(3))\n"
     "#define __DSPRegs ((vu16*)smgAudioRegisters())\n"
     + "".join(f'#define {name} (({kind}*)smgUnportedRegisters("{name}"))\n'
               for name, kind in [("__VIRegs", "vu16"), ("__AIRegs", "vu32"), ("__EXIRegs", "vu32"),
                                  ("__DIRegs", "vu32"), ("__SIRegs", "vu32"), ("__ACRRegs", "vu32"), ("__IPCRegs", "vu32")])
     + "#endif", 1),
])
# MSL's printf writes nothing for a null %s (printf.c: buff_ptr = ""), newlib writes "(null)": a folder of the
# archive's root must be found as "/name", as on the Wii.
adapt("src/Game/System/LayoutHolder.cpp", "Game/System/LayoutHolder.cpp", [
    ('                sprintf(path, "%s%s%s", pRoot, "/", pFinder->mName);',
     '                sprintf(path, "%s%s%s", pRoot ? pRoot : "", "/", pFinder->mName);', 1),
    ('                snprintf(path, 128, "%s/%s", pRoot, pFinder->mName);',
     '                snprintf(path, 128, "%s/%s", pRoot ? pRoot : "", pFinder->mName);', 1),
    # Each file turns native as its archive mounts (compat/smg/NativeResources.hpp).
    ('#include <JSystem/JKernel/JKRFileFinder.hpp>\n', '#include <JSystem/JKernel/JKRFileFinder.hpp>\n#include <smg/NativeResources.hpp>\n', 1),
    ('            pInfo->_C = pFinder->mFileID;\n', '            pInfo->_C = pFinder->mFileID;\n            smgNativeLayoutFile(pFinder->mName, pInfo->_8, pInfo->_4);\n', 1),
])
adapt("src/Game/System/ResourceHolder.cpp", "Game/System/ResourceHolder.cpp", [
    ('                sprintf(path, "%s%s%s", pPath, "/", finder->mName);',
     '                sprintf(path, "%s%s%s", pPath ? pPath : "", "/", finder->mName);', 1),
    ('                snprintf(path, 128, "%s/%s", pPath, finder->mName);',
     '                snprintf(path, 128, "%s/%s", pPath ? pPath : "", finder->mName);', 1),
])

# The top screen is 5:3 where the game's wide mode composes for 16:9 (832 columns, layouts at 0.75): these sites take
# compat/smg/Screen.hpp's 5:3 values, and the rest of screen space derives from MR::getScreenWidth.
adapt("src/Game/Util/ScreenUtil.cpp", "Game/Util/ScreenUtil.cpp", [
    ('#include "Game/Util/ScreenUtil.hpp"\n', '#include "Game/Util/ScreenUtil.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("        return isScreen16Per9() ? 832 : 608;", "        return isScreen16Per9() ? smg::screen::Width : 608;", 1),
])
adapt("src/Game/Camera/CameraContext.cpp", "Game/Camera/CameraContext.cpp", [
    ('#include "Game/Camera/CameraContext.hpp"\n', '#include "Game/Camera/CameraContext.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("        return 16.0f / 9.0f;", "        return smg::screen::Aspect;", 1),
])
adapt("src/Game/Camera/CameraViewInterpolator.cpp", "Game/Camera/CameraViewInterpolator.cpp", [
    ("    f32 nearPlaneWidth = nearPlaneHeight * 16.0f / 9.0f;", "    f32 nearPlaneWidth = nearPlaneHeight * MR::getAspect();", 1),
])
adapt("src/Game/Screen/ImageEffectLocalUtil.cpp", "Game/Screen/ImageEffectLocalUtil.cpp", [
    ('#include "Game/Screen/ImageEffectLocalUtil.hpp"\n', '#include "Game/Screen/ImageEffectLocalUtil.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("    f32 aspect = MR::isScreen16Per9() ? 1.333f : 1.0f;", "    f32 aspect = MR::isScreen16Per9() ? smg::screen::PixelAspect : 1.0f;", 1),
])
# The HOME Button Menu never opens here (source/compat/HomeButtonMenu.cpp); its adjust still follows the screen.
adapt("src/Game/Screen/HomeButtonLayout.cpp", "Game/Screen/HomeButtonLayout.cpp", [
    ("    mHomeButtonInfo->adjust.x = 832.0f / 608.0f;", "    mHomeButtonInfo->adjust.x = static_cast< f32 >(MR::getScreenWidth()) / 608.0f;", 1),
])
# A movie's 640x368 picture is framed for 16:9: the 5:3 screen shows it whole at its width, a 16th shorter, between
# bars that grow to match. (The 4:3 branch, which would crop instead, never runs here.)
adapt("src/Game/Screen/MoviePlayerSimple.cpp", "Game/Screen/MoviePlayerSimple.cpp", [
    ('#include "Game/Screen/MoviePlayerSimple.hpp"\n', '#include "Game/Screen/MoviePlayerSimple.hpp"\n#include <smg/Screen.hpp>\n', 1),
    ("    static const s16 sCinemaFrameHeightTop = 44;\n    static const s16 sCinemaFrameHeightBottom = 412;",
     "    static const s16 sCinemaFrameHeightTop = smg::screen::MovieBarTop;\n"
     "    static const s16 sCinemaFrameHeightBottom = smg::screen::MovieBarBottom;", 1),
    ("        u32 ySize = mMovie->mVideoInfo.ySize;\n", "        u32 ySize = mMovie->mVideoInfo.ySize * smg::screen::MovieHeightScale;\n", 1),
])

# A copy this script no longer writes would still be preferred over upstream's file by the build (and a stale
# header over upstream's by the compiler), so it goes; clean the build directories after this removes a header.
for stale in sorted(path for path in output.rglob("*") if path.is_file() and path.resolve() not in produced):
    stale.unlink()
    print(f"prepare-petari: removed {stale.relative_to(output)}, which it no longer generates")
