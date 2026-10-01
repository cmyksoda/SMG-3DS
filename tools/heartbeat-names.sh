#!/bin/sh
# Names the nerves in the probe's "heartbeat:" lines (source/compat/GameScene.cpp) from the ELF's symbols, latest last.
# Usage: tools/heartbeat-names.sh [log] [elf]
set -eu
log=${1:-$HOME/Documents/Artifacts/SMG-3DS/emulator/user/data/azahar-emu/log/azahar_log.txt}
elf=${2:-$HOME/Documents/Artifacts/SMG-3DS/3ds/mario_probe.elf}
dir=$(dirname "$elf")
lines=$(grep -a "heartbeat:" "$log" | sed 's/.*heartbeat: //' | tail -${COUNT:-6})
docker run --rm --user "$(id -u):$(id -g)" --mount "type=bind,source=$dir,target=/b,readonly" \
    devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
    sh -c "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm -C /b/$(basename "$elf") > /tmp/symbols.txt
echo \"$lines\" | while read -r line; do
  out=\"\$line\"
  for a in \$(echo \"\$line\" | grep -o '0x[0-9a-f]*'); do
    name=\$(grep -i \"^0*\${a#0x} \" /tmp/symbols.txt | head -1 | cut -d' ' -f3-)
    out=\$(echo \"\$out\" | sed \"s|\$a|\${name:-\$a}|\")
  done
  echo \"\$out\"
done"
