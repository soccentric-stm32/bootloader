#!/usr/bin/env bash
#
# provision.sh - put the NUCLEO-F767ZI into product configuration
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI over SWD
# SRS:     BHOST-003; SRS-BOOT-F767 BASM-001, BASM-002, section 2.5, OPEN-B05
#
# Sets nDBANK = 0 (dual-bank), nDBOOT = 1 (dual boot disabled), keeps the
# other option bytes, mass-erases the flash, writes BOOT and resets.
#
# Usage:
#   tools/provision.sh [build/boot.elf]
#
# Runs OpenOCD in the stm32-dev container through scripts/docker-dev.sh.
# Changing nDBANK forces a mass erase, so do not run this on a board whose
# flash contents you need to keep.  If OpenOCD reports that it cannot write
# the options (OPEN-B05), use STM32CubeProgrammer to set nDBANK and nDBOOT,
# then run this script again with --no-options.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
DEV="$ROOT/scripts/docker-dev.sh"
OPENOCD_CFG="${OPENOCD_CFG:-board/st_nucleo_f7.cfg}"

ELF=""
WRITE_OPTIONS=1
for arg in "$@"; do
    case "$arg" in
        --no-options) WRITE_OPTIONS=0 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) ELF="$arg" ;;
    esac
done

if [[ -z "$ELF" ]]; then
    ELF="$ROOT/build/boot.elf"
fi
if [[ ! -f "$ELF" ]]; then
    echo "provision: $ELF not found; run scripts/build.sh first" >&2
    exit 1
fi
ELF="$(realpath -m "$ELF")"

case "$ELF" in
    "$ROOT"/*) ELF_CONTAINER="/work/${ELF#"$ROOT"/}" ;;
    *) echo "provision: $ELF is outside the project directory" >&2; exit 1 ;;
esac

run_openocd() {
    "$DEV" run openocd -f "$OPENOCD_CFG" "$@" 2>&1
}

if [[ "$WRITE_OPTIONS" -eq 1 ]]; then
    echo "[*] reading option bytes"
    read_output="$(run_openocd -c "init; reset halt; stm32f2x options_read 0; shutdown")" || true
    echo "$read_output"

    user_options="$(sed -n 's/.*user_options 0x\([0-9A-Fa-f]*\).*/\1/p' <<<"$read_output" | head -1)"
    boot_add0="$(sed -n 's/.*boot_add0 0x\([0-9A-Fa-f]*\).*/\1/p' <<<"$read_output" | head -1)"
    boot_add1="$(sed -n 's/.*boot_add1 0x\([0-9A-Fa-f]*\).*/\1/p' <<<"$read_output" | head -1)"
    if [[ -z "$user_options" || -z "$boot_add0" || -z "$boot_add1" ]]; then
        echo "provision: could not read the option bytes" >&2
        echo "provision: check the ST-LINK connection and OPEN-B05" >&2
        exit 1
    fi

    # BASM-002: BOOT_ADD0 is 0x0080 (flash via ITCM, bank 1 start). Keep
    # BOOT_ADD1 untouched: its default 0x0040 is the ST system-memory
    # bootloader, the recovery path when BOOT0 is high. Writing 0x0000
    # would point BOOT0 at ITCM RAM and lose that path.
    old=$((16#$user_options))
    # bit 8 = nDBOOT = 1, bit 9 = nDBANK = 0; keep the other user options.
    new=$(( (old & ~0x300) | 0x100 ))
    new_hex="$(printf '0x%X' "$new")"
    boot0="$boot_add0"
    if [[ $((16#$boot_add0)) -ne $((16#0080)) ]]; then
        boot0="0080"
    fi
    boot1="$boot_add1"

    echo "[*] option bytes: user_options 0x${user_options} -> ${new_hex} (nDBANK=0, nDBOOT=1), boot_add0 0x${boot_add0} -> 0x${boot0}, boot_add1 0x${boot1} (kept)"
    write_output="$(run_openocd -c "init; reset halt; stm32f2x options_write 0 ${new_hex} 0x${boot0} 0x${boot1}; shutdown")" || true
    echo "$write_output"

    if ! grep -q "write options complete" <<<"$write_output"; then
        echo "provision: OpenOCD could not write the option bytes (OPEN-B05)." >&2
        echo "provision: set nDBANK=0 and nDBOOT=1 with STM32CubeProgrammer," >&2
        echo "provision: then rerun: tools/provision.sh --no-options $ELF" >&2
        exit 1
    fi
fi

echo "[*] mass erase"
run_openocd -c "init; reset halt; stm32f2x mass_erase 0; shutdown"

echo "[*] flashing $ELF"
run_openocd -c "init; reset halt; program $ELF_CONTAINER verify; reset run; shutdown"

echo "[*] provisioning complete"
