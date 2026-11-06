"""Read the version and size from a BOOT image header for display."""

from __future__ import annotations

import struct
from pathlib import Path

IMAGE_MAGIC = 0x48574249
IMAGE_PREFIX = struct.Struct("<IHHIII")


def human_size(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f} MB"
    if size >= 1024:
        return f"{size / 1024:.1f} KB"
    return f"{size} B"


def version_text(value: int) -> str:
    return f"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{value & 0xFF}"


def image_summary(path: Path) -> str | None:
    """'v1.2.3 · 640.0 KB' from a BOOT image header, or None."""
    try:
        with path.open("rb") as handle:
            head = handle.read(IMAGE_PREFIX.size)
    except OSError:
        return None
    if len(head) < IMAGE_PREFIX.size:
        return None
    magic, _hv, _hs, _type, version, size = IMAGE_PREFIX.unpack(head)
    if magic != IMAGE_MAGIC:
        return None
    return f"v{version_text(version)} · {human_size(size)}"
