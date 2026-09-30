#!/usr/bin/env bash
# Syntax-checks every Petari game source for the 3DS in the port's include order, one line per file:
# path, error count, first error. Needs a configured 3DS build dir for the generated headers.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(cd -- "${1:?usage: petari-syntax-scan.sh 3DS_BUILD_DIR OUTPUT.tsv}" && pwd)
output=${2:?usage: petari-syntax-scan.sh 3DS_BUILD_DIR OUTPUT.tsv}
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
check='f="$1"
out=$(arm-none-eabi-g++ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -fno-rtti -fno-exceptions \
  -std=c++17 -fsyntax-only -funsigned-char -fno-strict-aliasing -w -fmax-errors=50 -include /src/compat/smg/NewlibCtype.hpp \
  -I/src/compat -isystem /opt/devkitpro/libctru/include -isystem /build/petari/include -isystem /src/upstream/Petari/include \
  -isystem /src/upstream/Petari/libs/RVL_SDK/include -isystem /src/upstream/Petari/libs/JSystem/include \
  -isystem /src/upstream/Petari/libs/nw4r/include "$f" 2>&1 || true)
first=$(printf "%s" "$out" | grep -m1 "error:" | sed "s/^[^:]*:[0-9]*:[0-9]*: error: //" | cut -c1-140)
printf "%s\t%s\t%s\n" "${f#/src/upstream/Petari/src/}" "$(printf "%s" "$out" | grep -c "error:")" "$first"'
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build,readonly" \
  -e PATH=/opt/devkitpro/devkitARM/bin:/usr/bin:/bin "$builder_image" \
  bash -c "find /src/upstream/Petari/src/Game -name '*.cpp' | sort | xargs -P \"\$(nproc)\" -n 1 bash -c '$check' check" > "$output"
printf '%s of %s pass\n' "$(awk -F'\t' '$2 == 0' "$output" | wc -l)" "$(wc -l < "$output")"
