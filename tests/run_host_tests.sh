#!/usr/bin/env bash
# Build and run the host unit tests (P0-1/P0-2, torn records, trial limit).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
make -C "$ROOT/tests/host" clean >/dev/null
make -C "$ROOT/tests/host" run
