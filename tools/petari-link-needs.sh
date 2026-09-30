#!/usr/bin/env bash
# Compiles every source under the given Petari directories for the 3DS (generated copies where the port adapts
# one) and lists what they reference that nothing defines: a bring-up's work list. symbols.txt feeds petari-closure.py.
# With a header and a statement, it also links a program running that statement against the port's libraries,
# dropping unreached functions as the real link will: probe-undefined.txt and probe-duplicates.txt say what's left.
set -euo pipefail
usage='usage: petari-link-needs.sh 3DS_BUILD_DIR "Game JSystem" OUT_DIR [Game/Player/MarioActor.hpp "new MarioActor(\"m\");"]'
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(cd -- "${1:?$usage}" && pwd)
subdirs=${2:?$usage}
mkdir -p "${3:?$usage}"
out_dir=$(cd -- "$3" && pwd)
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
rm -f "$out_dir/failed.txt" "$out_dir"/probe-*.txt
docker run --rm --network none --user "$(id -u):$(id -g)" -i \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build,readonly" \
  --mount "type=bind,source=$out_dir,target=/out" \
  -e PATH=/opt/devkitpro/devkitARM/bin:/usr/bin:/bin -e SUBDIRS="$subdirs" \
  -e PROBE_HEADER="${4:-}" -e PROBE_BODY="${5:-}" "$builder_image" bash -s <<'SCRIPT'
set -uo pipefail
P=/src/upstream/Petari
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
FLAGS="$ARCH -D__3DS__ -fno-rtti -fno-exceptions -std=c++17 -c -O1 -w -ffunction-sections -fdata-sections
  -funsigned-char -fno-strict-aliasing -include /src/compat/smg/NewlibCtype.hpp -include /src/compat/smg/Msl.hpp -Wno-narrowing
  -I/src/compat -I/src/include -isystem /opt/devkitpro/libctru/include -isystem /build/petari/include -isystem $P/include
  -isystem $P/libs/RVL_SDK/include -isystem $P/libs/JSystem/include -isystem $P/libs/nw4r/include -isystem $P/libs/RVLFaceLib/include"
cd /out && rm -f -- *.o game.a
compile() {
    relative=${1#$P/src/}; src=$1; object=$(echo "${relative%.cpp}" | tr / _).o
    [ -f "/build/petari/$relative" ] && src=/build/petari/$relative
    arm-none-eabi-g++ $FLAGS "$src" -o "$object" 2>/dev/null || echo "$relative" >> failed.txt
}
export -f compile; export P FLAGS
for d in $SUBDIRS; do find "$P/src/$d" -name "*.cpp"; done | xargs -P "$(nproc)" -n 1 bash -c 'compile "$0"'
arm-none-eabi-nm -C --defined-only -- *.o /build/lib*.a 2>/dev/null |
    awk '$2 ~ /^[TWVDBRtwvdbruS]$/ {sub(/^[0-9a-f]+ [A-Za-z] /,""); print}' | sort -u > defined.txt
arm-none-eabi-nm -C --undefined-only -- *.o | sed "s/^ *U //" | grep -v ":$" |
    grep -v "^\(std::\|operator \|__aeabi\|__cxa\|__gxx\|mem\|str\|sqrt\|sin\|cos\|tan\|atan\|acos\|asin\|fmod\|pow\|floor\|ceil\|abort\|printf\|snprintf\|vsnprintf\|sprintf\|fabs\|__sync\|_Unwind\|__stack_chk\)" |
    sort -u > needed.txt
comm -23 needed.txt defined.txt > missing.txt
arm-none-eabi-nm -A -- *.o /build/lib*.a 2>/dev/null > symbols.txt
if [ -n "$PROBE_HEADER" ]; then
    arm-none-eabi-ar rcs game.a -- *.o
    printf '#include <%s>\nint main() {\n    %s\n    return 0;\n}\n' "$PROBE_HEADER" "$PROBE_BODY" > probe.cpp
    arm-none-eabi-g++ $FLAGS probe.cpp -o probe.o
    arm-none-eabi-g++ -specs=3dsx.specs $ARCH probe.o -Wl,--gc-sections -Wl,--start-group /build/lib*.a game.a -Wl,--end-group \
        -L/opt/devkitpro/libctru/lib -lcitro2d -lcitro3d -lctru -lm -o probe.elf > probe-link.txt 2>&1
    grep -o "undefined reference to \`[^']*'" probe-link.txt | sed "s/^undefined reference to \`//; s/'$//" | sort | uniq -c | sort -rn > probe-undefined.txt
    grep -o "multiple definition of \`[^']*'" probe-link.txt | sed "s/^multiple definition of \`//; s/'$//" | sort -u > probe-duplicates.txt
    rm -f probe.o probe.elf game.a
fi
rm -f -- *.o
SCRIPT
printf '%s failed to compile, %s symbols missing (%s)\n' "$( [ -f "$out_dir/failed.txt" ] && wc -l < "$out_dir/failed.txt" || echo 0)" \
  "$(wc -l < "$out_dir/missing.txt")" "$out_dir/missing.txt"
if [ -n "${4:-}" ]; then
    printf 'probe link: %s symbols undefined, %s defined twice (%s)\n' "$(wc -l < "$out_dir/probe-undefined.txt")" \
      "$(wc -l < "$out_dir/probe-duplicates.txt")" "$out_dir/probe-link.txt"
fi
