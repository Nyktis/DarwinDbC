#!/bin/bash
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
mkdir -p "$HERE/captures"
exec "$HERE/darwindbc-receive" --output "$HERE/captures/$(date +%Y%m%d-%H%M%S)-$$.log"
