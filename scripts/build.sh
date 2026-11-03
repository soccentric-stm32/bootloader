#!/usr/bin/env bash
#
# build.sh - Configure and build the A/B bootloader inside the stm32-dev container (BCON-001)
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Usage:   scripts/build.sh            configure (if needed) + build
#          scripts/build.sh clean      rm -rf build, then configure + build
#
# Outputs: build/boot.elf, build/boot.bin, build/boot.hex, build/boot.map
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
DEV="$ROOT/scripts/docker-dev.sh"
cd "$ROOT"

if [[ "${1:-}" == "clean" ]]; then
    rm -rf build
fi

"$DEV" run cmake -S /work -B /work/build -G Ninja \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
"$DEV" run cmake --build /work/build
