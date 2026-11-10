#!/bin/bash
set -euo pipefail
if [ "$(uname -s)" != Darwin ]; then
    echo 'Run this on the target macOS after reaching the desktop.' >&2
    exit 2
fi
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
RUN="$HERE/target-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN"
sysctl kern.bootargs > "$RUN/boot-args.txt"
ioreg -r -c DarwinDbC -l -w 0 > "$RUN/bootstrap.txt"
ioreg -r -c AppleUSBXHCI -l -w 0 > "$RUN/controllers.txt"
ioreg -r -c DarwinDbCService -l -w 0 > "$RUN/service-before.txt"
sleep 2
ioreg -r -c DarwinDbCService -l -w 0 > "$RUN/service-after.txt"
cat "$RUN/service-after.txt"
printf '\nSaved target connection state: %s\n' "$RUN"
