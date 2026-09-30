#!/usr/bin/env bash
# Compiles every source under one Petari directory for the 3DS (generated copies where the port adapts one) and
# lists what they reference that neither they nor the port's libraries define: a bring-up's work list.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(cd -- "${1:?usage: petari-link-needs.sh 3DS_BUILD_DIR Game/Player OUT_DIR}" && pwd)
subdir=${2:?usage: petari-link-needs.sh 3DS_BUILD_DIR Game/Player OUT_DIR}
mkdir -p "${3:?usage: petari-link-needs.sh 3DS_BUILD_DIR Game/Player OUT_DIR}"
out_dir=$(cd -- "$3" && pwd)
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
script='
P=/src/upstream/Petari
FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -fno-rtti -fno-exceptions -std=c++17 -c -O1 -w
  -funsigned-char -fno-strict-aliasing -include /src/compat/smg/NewlibCtype.hpp -I/src/compat -I/src/include
  -isystem /opt/devkitpro/libctru/include -isystem /build/petari/include -isystem $P/include
  -isystem $P/libs/RVL_SDK/include -isystem $P/libs/JSystem/include -isystem $P/libs/nw4r/include"
cd /out && rm -f -- *.o
compile() {
    relative=${1#$P/src/}; src=$1; object=$(echo "${relative%.cpp}" | tr / _).o
    [ -f "/build/petari/$relative" ] && src=/build/petari/$relative
    arm-none-eabi-g++ $FLAGS "$src" -o "$object" 2>/dev/null || echo "$relative" >> failed.txt
}
export -f compile; export P FLAGS
find "$P/src/$SUBDIR" -name "*.cpp" | xargs -P "$(nproc)" -n 1 bash -c '"'"'compile "$0"'"'"'
arm-none-eabi-nm -C --defined-only -- *.o /build/lib*.a 2>/dev/null |
    awk '"'"'$2 ~ /^[TWVDBRtwvdbruS]$/ {sub(/^[0-9a-f]+ [A-Za-z] /,""); print}'"'"' | sort -u > defined.txt
arm-none-eabi-nm -C --undefined-only -- *.o | sed "s/^ *U //" | grep -v ":$" |
    grep -v "^\(std::\|operator \|__aeabi\|__cxa\|__gxx\|mem\|str\|sqrt\|sin\|cos\|tan\|atan\|acos\|asin\|fmod\|pow\|floor\|ceil\|abort\|printf\|snprintf\|vsnprintf\|sprintf\|fabs\|__sync\|_Unwind\|__stack_chk\)" |
    sort -u > needed.txt
comm -23 needed.txt defined.txt > missing.txt
rm -f -- *.o'
rm -f "$out_dir/failed.txt"
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build,readonly" \
  --mount "type=bind,source=$out_dir,target=/out" \
  -e PATH=/opt/devkitpro/devkitARM/bin:/usr/bin:/bin -e SUBDIR="$subdir" "$builder_image" bash -c "$script"
printf '%s failed to compile, %s symbols missing (%s)\n' "$( [ -f "$out_dir/failed.txt" ] && wc -l < "$out_dir/failed.txt" || echo 0)" \
  "$(wc -l < "$out_dir/missing.txt")" "$out_dir/missing.txt"
