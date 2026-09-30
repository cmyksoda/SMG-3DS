#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${SMG_HOST_BUILD_DIR:-"$HOME/.cache/smg-3ds/host"}
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
mkdir -p "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
# The devkitARM image also carries a host GCC with ASan/UBSan, which not every distro installs.
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build" \
  "$builder_image" bash -c \
  'cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" >/dev/null &&
   cmake --build /build && ctest --test-dir /build --output-on-failure'
