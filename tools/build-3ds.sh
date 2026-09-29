#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${SMG_BUILD_DIR:-"$HOME/.cache/smg-3ds/3ds"}
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
mkdir -p "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
docker run --rm --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build" \
  "$builder_image" bash -c \
  'cmake -S /src -B /build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/src/tools/3ds-toolchain.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build /build'
printf 'Built %s/SMG-3DS.3dsx\n' "$build_dir"
