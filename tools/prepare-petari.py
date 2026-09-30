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


def adapt(relative, target, replacements, drop=(), reason=""):
    """Copy a file with exact replacements; `drop` leaves out whole definitions, found by first line, for `reason`."""
    text = (root / relative).read_text()
    for before, after, expected in replacements:
        if text.count(before) != expected:
            raise SystemExit(f"Upstream changed; review {relative}: {before!r}")
        text = text.replace(before, after)
    for first in drop:
        start, end = definition(text, relative, first)
        indent = text[start:text.index(first)]
        text = text[:start] + f"{indent}// Left out, {reason}: {first.rstrip(' {')}" + text[end:]
    destination = output / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    text = "// Generated Petari adaptation; see tools/prepare-petari.py.\n" + text
    if not destination.exists() or destination.read_text() != text:
        destination.write_text(text)


def extract(relative, target, prologue, first_lines, epilogue=""):
    """Copy whole function definitions, found by their first line, out of a file too entangled to compile.

    A (namespace, first line) pair copies a definition that sits inside that namespace's block."""
    text = (root / relative).read_text()
    bodies = []
    for first in first_lines:
        space, first = first if isinstance(first, tuple) else (None, first)
        start, end = definition(text, relative, first)
        bodies.append(f"namespace {space} {{\n{text[start:end]}\n}}  // namespace {space}" if space else text[start:end])
    text = (f"// Generated from Petari's {relative}: only these functions; see tools/prepare-petari.py.\n"
            + prologue + "\n" + "\n\n".join(bodies) + "\n" + epilogue)
    destination = output / target
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_text() != text:
        destination.write_text(text)


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
    ("inline TVec2& addInline", "inline TVec2 addInline", 1),
    ("inline const TVec2& addOperatorInline", "inline TVec2 addOperatorInline", 1),
    ("        TVec3(const Vec& vec);", "        TVec3(const Vec& vec) : Vec(vec) {}", 1),
    ("        TVec3(const TVec3< f32 >& vec);", "        TVec3(const TVec3< f32 >& vec) : Vec(vec) {}", 1),
    ("            stfs b_x, 8(v_b)\n            }\n            ;\n#endif", "            stfs b_x, 8(v_b)\n            }\n            ;\n#else\n            static_cast<Vec&>(*this) = static_cast<const Vec&>(vec);\n#endif", 1),
    ("        void setPSZeroVec();", "        void setPSZeroVec() { zero(); }", 1),
    ("            return _fp1;\n#endif", "            return _fp1;\n#else\n            return JMathInlineVEC::PSVECDotProduct(this, &rOther);\n#endif", 1),
    ("void scaleAdd(register f32 scale", "void scaleAdd(f32 scale", 1),
    ("        f32 squared(const TVec3& rB) const;", "        f32 squared(const TVec3& rB) const { return JMathInlineVEC::PSVECSquareDistance(this, &rB); }", 1),
    ("__fabsf(JMAATan2", "std::fabs(JMAATan2", 1),
    ("                    f32 estimate = __frsqrte(length);\n                    inverse = (0.5f * estimate) * (3.0f - length * (estimate * estimate));", "                    inverse = 1.0f / std::sqrt(length);", 1),
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
    ('#include "Game/LiveActor/LiveActor.hpp"\n', "", 1),
    ("#include <cstring>", "#include <cstdint>\n#include <cstring>", 1),
    ("u32 host) NO_INLINE {", "u32 host) {", 1),
    # A u32 holds a pointer on the Wii and the 3DS; 64-bit host tests keep the low half, as the manager compares.
    ("host = (u32)pActor;", "host = (u32)(uintptr_t)pActor;", 1),
], drop=[
    "bool calcGravityVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool calcDropShadowVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool calcGravityAndDropShadowVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool calcGravityVectorOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool calcGravityAndDropShadowVectorOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool calcAttractMarioLauncherOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, u32 host) {",
    "bool isZeroGravity(const LiveActor* pActor) {",
], reason="LiveActor is not brought up")

# Wire gravity follows a rail; RailRider and the stage's path data are not brought up.
adapt("include/Game/Gravity/GravityCreator.hpp", "include/Game/Gravity/GravityCreator.hpp", [
    ('#include "Game/LiveActor/RailRider.hpp"', "class RailRider;", 1),
])
adapt("src/Game/Gravity/GravityCreator.cpp", "Game/Gravity/GravityCreator.cpp", [
    ('#include "Game/Gravity.hpp"\n#include "Game/Util.hpp"',
     '#include "Game/Gravity/GravityCreator.hpp"\n#include "Game/Util/GravityUtil.hpp"\n#include "Game/Util/JMapUtil.hpp"\n'
     '#include "Game/Util/MathUtil.hpp"\n#include "Game/Util/MtxUtil.hpp"', 1),
], drop=[
    "PlanetGravity* WireGravityCreator::createInstance() {",
    "void WireGravityCreator::settingFromJMapOtherParam(const JMapInfoIter& rIter) {",
    "PlanetGravity* WireGravityCreator::getGravity() {",
], reason="RailRider is not brought up")

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
adapt("src/Game/Map/CollisionCode.cpp", "Game/Map/CollisionCode.cpp", [
    ('#include "Game/Map/HitInfo.hpp"', '#include "Game/Util/JMapInfo.hpp"', 1),
], drop=[
    "const char* CollisionCode::getFloorCodeString(const Triangle& rTriangle) {",
    "const char* CollisionCode::getWallCodeString(const Triangle& rTriangle) {",
    "const char* CollisionCode::getSoundCodeString(const Triangle& rTriangle) {",
    "u32 CollisionCode::getCameraID(const Triangle& rTriangle) {",
], reason="Triangle comes with CollisionParts, which waits for LiveActor")

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
