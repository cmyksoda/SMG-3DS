#!/bin/sh
# Names the functions a stop's "callers on the stack" line points into (source/probe/MarioProbe.cpp), newest first.
# Some may be stale words from earlier calls; the first few are the stop's own chain.
# Usage: tools/stop-callers.sh [log] [elf]
set -eu
log=${1:-$HOME/Documents/Artifacts/SMG-3DS/emulator/user/data/azahar-emu/log/azahar_log.txt}
elf=${2:-$HOME/Documents/Artifacts/SMG-3DS/3ds/mario_probe.elf}
addresses=$(grep -a "callers on the stack:" "$log" | tail -1 | sed 's/.*callers on the stack://')
[ -n "$addresses" ] || { echo "no callers line in $log" >&2; exit 1; }
dir=$(dirname "$elf")
docker run --rm --user "$(id -u):$(id -g)" --mount "type=bind,source=$dir,target=/b,readonly" \
    devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
    sh -c "for a in $addresses; do printf '%s ' \$a; /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -C -s -e /b/$(basename "$elf") \$(printf '%x' \$((0x\$a - 4))) | paste -sd' '; done"
