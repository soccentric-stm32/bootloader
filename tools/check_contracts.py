#!/usr/bin/env python3
#
# check_contracts.py - keep the boot contract and generated schema in sync
#
# Checks this repository and, with --app-repo, the copies used by the C,
# Rust and Zephyr applications (P3-8, section 4 of IMPROVEMENTS.md).
#
# Usage:
#   tools/check_contracts.py [--app-repo ../app-c-freertos]

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

LAYOUT_NAMES = (
    "BOOT_BANK1_BASE", "BOOT_BANK2_BASE", "BOOT_META_OFFSET",
    "BOOT_META_SIZE", "BOOT_SLOT_OFFSET", "BOOT_SLOT_SIZE",
    "BOOT_META_VERIFIED_OFFSET",
    "BOOT_META_ACTIVATED_OFFSET", "BOOT_META_CONFIRMED_OFFSET",
    "BOOT_META_REJECTED_OFFSET", "BOOT_META_TRIAL_OFFSET",
)


def defines(path: pathlib.Path) -> dict[str, int]:
    result: dict[str, int] = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define":
            value = parts[2].rstrip("ULul")
            try:
                result[parts[1]] = int(value, 0)
            except ValueError:
                continue
    return result


def check_repo(root: pathlib.Path, label: str) -> list[str]:
    errors: list[str] = []
    layout = root / "src" / "common" / "boot_layout.h"
    if not layout.is_file():
        layout = root / "src" / "app" / "boot_contract.h"
    if not layout.is_file():
        layout = root / "boot_contract.h"
    if not layout.is_file():
        errors.append(f"{label}: no boot_layout.h or boot_contract.h")
        return errors

    values = defines(layout)
    text = layout.read_text(encoding="utf-8", errors="replace")
    if "BOOT_APP_LOAD_ADDR" not in text and "0x0800C000" not in text:
        errors.append(f"{label}: application load address is not published")
    for name in LAYOUT_NAMES:
        if name not in values:
            errors.append(f"{label}: {name} missing from {layout}")
    expected = defines(ROOT / "src" / "common" / "boot_layout.h")
    for name, value in expected.items():
        if name in LAYOUT_NAMES and name in values and values[name] != value:
            errors.append(
                f"{label}: {name}=0x{values[name]:X}, "
                f"BOOT has 0x{value:X}")

    memory_files = list(root.rglob("memory.x"))
    if memory_files:
        memory_text = memory_files[0].read_text(
            encoding="utf-8", errors="replace").lower()
        if "0x0800c000" not in memory_text and "0x800c000" not in memory_text:
            errors.append(
                f"{label}: {memory_files[0]} does not link at 0x0800C000")

    schema = root / "protocol" / "hwlink.fbs"
    if not schema.is_file():
        schema = root / "hwlink.fbs"
    if schema.is_file():
        text = schema.read_text(encoding="utf-8", errors="replace")
        for field in ("header_crc32", "active_valid", "target_slot",
                      "force"):
            if field not in text:
                errors.append(f"{label}: hwlink.fbs lacks {field}")
    else:
        errors.append(f"{label}: hwlink.fbs not found")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-repo", action="append", default=[],
                        help="application repository to compare")
    args = parser.parse_args()

    errors = check_repo(ROOT, "BOOT")

    # Generated protocol must expose the minor-2 fields and the Python and
    # C copies must agree on the protocol minor.
    bootinfo = (ROOT / "tools" / "generated" / "hwlink" /
                "BootInfo.py").read_text(encoding="utf-8")
    for method in ("ActiveValid", "TargetSlot", "BootVersion", "BootBuild"):
        if f"def {method}(" not in bootinfo:
            errors.append(f"generated BootInfo.py lacks {method}")
    update_begin = (ROOT / "tools" / "generated" / "hwlink" /
                    "UpdateBegin.py").read_text(encoding="utf-8")
    if "def Force(" not in update_begin:
        errors.append("generated UpdateBegin.py lacks Force")

    c_minor = re.search(r"HWLINK_PROTOCOL_MINOR\s+(\d+)U",
                        (ROOT / "src" / "protocol" / "hwlink.h").read_text())
    py_minor = re.search(r"PROTOCOL_MINOR = (\d+)",
                         (ROOT / "tools" / "bootctl.py").read_text())
    if not c_minor or not py_minor:
        errors.append("could not read the protocol minor version")
    elif c_minor.group(1) != py_minor.group(1):
        errors.append(f"protocol minor mismatch: C {c_minor.group(1)}, "
                      f"Python {py_minor.group(1)}")
    if c_minor and c_minor.group(1) != "2":
        errors.append("protocol minor is not 2 (P0-3/P0-1 fields)")

    # P1-4: factory metadata layout must stay identical to Appendix B.
    import mkimage  # noqa: E402
    boot_values = defines(ROOT / "src" / "common" / "boot_layout.h")
    for name in LAYOUT_NAMES:
        short = name.removeprefix("BOOT_")
        if hasattr(mkimage, short) and name in boot_values and \
                getattr(mkimage, short) != boot_values[name]:
            errors.append(f"mkimage.{short} != BOOT {name}")
    header = bytes(256)
    sector = mkimage.build_metadata(header, 0x11223344, 1)
    if len(sector) != 0x4000:
        errors.append("metadata sector is not 16 KB")
    for offset, magic in ((0x100, 0x56455249), (0x120, 0x41435456),
                          (0x140, 0x434F4E46)):
        if int.from_bytes(sector[offset:offset + 4], "little") != magic:
            errors.append(f"metadata record at 0x{offset:03X} is wrong")

    for repo in args.app_repo:
        errors.extend(check_repo(pathlib.Path(repo).resolve(), repo))

    if errors:
        for error in errors:
            print(f"contract: {error}", file=sys.stderr)
        return 1
    print(f"contract: layout, schema and protocol minor 2 agree"
          + (f" across {len(args.app_repo)} application repo(s)"
             if args.app_repo else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
