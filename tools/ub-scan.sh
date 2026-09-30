#!/usr/bin/env bash
# Recompiles every Petari object of a configured 3DS build with the warnings that name reads past an array's end,
# which gcc may treat as unreachable and fold away (docs/PORTING.md). Prints each warning once; exits 1 if any.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(cd -- "${SMG_BUILD_DIR:-"$HOME/.cache/smg-3ds/3ds"}" && pwd)
builder_image=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
docker run --rm --network none --user "$(id -u):$(id -g)" \
  --mount "type=bind,source=$project_dir,target=/src,readonly" \
  --mount "type=bind,source=$build_dir,target=/build,readonly" \
  "$builder_image" bash -c '
cd /build
ninja -t commands | grep -E "CMakeFiles/petari_[a-z0-9_]+\.dir/.*\.obj " | grep -- " -c " |
  sed "s| -MD -MT [^ ]* -MF [^ ]*||; s| -o [^ ]*| -Wall -Wno-unused -Wno-sign-compare -Wno-reorder -o /dev/null|" > /tmp/commands
xargs -P "$(nproc)" -d "\n" -I{} sh -c "{} 2>&1 || echo SCAN-FAILED" < /tmp/commands > /tmp/output || true
grep -E "invokes undefined behavior|Warray-bounds|is out of the bounds|Wstringop-overread|Wstringop-overflow" /tmp/output | sort -u > /tmp/found || true
failed=$(grep -c "^SCAN-FAILED" /tmp/output || true)
cat /tmp/found
printf "%s objects scanned, %s did not compile, %s warnings\n" "$(wc -l < /tmp/commands)" "$failed" "$(wc -l < /tmp/found)"
[ ! -s /tmp/found ] && [ "$failed" = 0 ]'
