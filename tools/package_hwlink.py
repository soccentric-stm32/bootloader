#!/usr/bin/env python3
"""Bundle the canonical HWLINK contract for the application repositories.

P3-8: hwlink.fbs, the generated Python package, mkimage.py, boot_layout.h
and the vendored Python flatbuffers runtime are published from this one
place.  Application repositories can consume the resulting directory (or a
CI artifact) instead of carrying drifting copies.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "build" / "soccentric-hwlink"


def copy_file(source: pathlib.Path, dest: pathlib.Path) -> pathlib.Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)
    return dest


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package(output: pathlib.Path) -> int:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    copy_file(ROOT / "protocol" / "hwlink.fbs",
              output / "protocol" / "hwlink.fbs")
    copy_file(ROOT / "src" / "common" / "boot_layout.h",
              output / "include" / "boot_layout.h")
    copy_file(ROOT / "tools" / "mkimage.py", output / "mkimage.py")

    shutil.copytree(ROOT / "tools" / "generated" / "hwlink",
                    output / "hwlink",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copytree(ROOT / "tools" / "vendor" / "flatbuffers",
                    output / "flatbuffers",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    readme = output / "README.md"
    readme.write_text(
        "# soccentric-hwlink\n\n"
        "Canonical HWLINK contract and host image tool, published from\n"
        "`bootloader`.\n\n"
        "Contents:\n"
        "- `protocol/hwlink.fbs` - FlatBuffers schema (protocol minor 2)\n"
        "- `hwlink/` - generated Python FlatBuffers package\n"
        "- `flatbuffers/` - vendored Python runtime\n"
        "- `mkimage.py` - application image and factory metadata builder\n"
        "- `include/boot_layout.h` - flash/slot/metadata constants\n\n"
        "Regenerate the C/Python bindings from `hwlink.fbs` after a schema\n"
        "change, then run `tools/check_contracts.py` in the bootloader repo\n"
        "and in each application repo.\n",
        encoding="utf-8")

    files = sorted(path for path in output.rglob("*") if path.is_file())
    sums = output / "SHA256SUMS"
    with sums.open("w", encoding="utf-8") as handle:
        for path in files:
            if path == sums:
                continue
            handle.write(f"{sha256(path)}  {path.relative_to(output)}\n")

    print(f"package_hwlink: {output} ({len(files)} files)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    return package(args.output)


if __name__ == "__main__":
    sys.exit(main())
