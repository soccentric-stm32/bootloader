#!/usr/bin/env python3
#
# mkimage.py - wrap an application binary in a BOOT image file
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI
# SRS:     BHOST-001, BHOST-002; SRS-BOOT-F767 section 5.1
#
# Usage:
#   tools/mkimage.py --input build/app.bin --output build/app.img \
#       --version 0.1.0 --commit <40-hex> --timestamp <unix> \
#       --name stm32-application
#
#   tools/mkimage.py --inspect build/app.img
#
#   tools/mkimage.py --image build/app.img --metadata A \
#       --output build/meta_a.bin
#   tools/mkimage.py --factory-hex build/factory.hex --boot build/boot.hex \
#       --slot-a build/app_a.img --slot-b build/app_b.img
#
# The header is 256 bytes; the binary follows unchanged.  BOOT stores this
# header in the slot metadata sector (IMG-006).

import argparse
import pathlib
import struct
import sys
import zlib

MAGIC = 0x48574249          # "IBWH" little-endian
HEADER_VERSION = 1
HEADER_SIZE = 256
IMAGE_TYPE_APPLICATION = 1
LOAD_ADDRESS = 0x0800C000
SLOT_SIZE = 0x000F4000      # 976 KB
RAM_START = 0x20000000
RAM_END = 0x20080000

# P1-4: physical layout used by the factory images.
BANK1_BASE = 0x08000000
BANK2_BASE = 0x08100000
BOOT_REGION_SIZE = 0x00008000
META_OFFSET = 0x00008000
META_SIZE = 0x00004000
SLOT_OFFSET = 0x0000C000
SLOT_A_BASE = BANK1_BASE + SLOT_OFFSET
SLOT_B_BASE = BANK2_BASE + SLOT_OFFSET
META_A_BASE = BANK1_BASE + META_OFFSET
META_B_BASE = BANK2_BASE + META_OFFSET
BOOT_BASE = BANK1_BASE

META_VERIFIED_OFFSET = 0x100
META_ACTIVATED_OFFSET = 0x120
META_CONFIRMED_OFFSET = 0x140
META_REJECTED_OFFSET = 0x160
META_RECORD_SIZE = 32
META_VERIFIED_MAGIC = 0x56455249
META_ACTIVATED_MAGIC = 0x41435456
META_CONFIRMED_MAGIC = 0x434F4E46
META_REJECTED_MAGIC = 0x52454A45

IMAGE_HEADER = struct.Struct("<IHHIIIIIII20s32s164sI")

HEADER_FIELDS = (
    ("magic", "<I"),
    ("header_version", "<H"),
    ("header_size", "<H"),
    ("image_type", "<I"),
    ("version", "<I"),
    ("image_size", "<I"),
    ("image_crc32", "<I"),
    ("load_address", "<I"),
    ("entry_address", "<I"),
    ("build_timestamp", "<I"),
)


def semver(value: str) -> int:
    parts = value.split(".")
    if len(parts) != 3:
        raise ValueError(f"version must be major.minor.patch, got {value!r}")
    major, minor, patch = (int(p) & 0xFF for p in parts)
    return (major << 24) | (minor << 16) | patch


def version_text(value: int) -> str:
    return f"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{value & 0xFF}"


def commit_bytes(value: str) -> bytes:
    raw = bytes(20)
    text = value.strip()
    if len(text) >= 40:
        try:
            raw = bytes.fromhex(text[:40])
        except ValueError:
            raw = text[:20].encode("ascii", "replace").ljust(20, b"\0")
    elif text:
        raw = text[:20].encode("ascii", "replace").ljust(20, b"\0")
    return raw


def commit_text(raw: bytes) -> str:
    if all(b == 0 for b in raw):
        return ""
    return raw.hex()


def validate_binary(binary: bytes) -> list[str]:
    errors = []
    if len(binary) < 8:
        errors.append("input binary is too small for a vector table")
        return errors
    if len(binary) > SLOT_SIZE:
        errors.append(f"binary is {len(binary)} bytes; the slot is {SLOT_SIZE}")

    sp, entry = struct.unpack_from("<II", binary, 0)
    if not (RAM_START < sp <= RAM_END) or (sp & 0x7):
        errors.append(f"initial SP 0x{sp:08x} is not in internal RAM")
    entry_addr = entry & ~1
    if not (entry & 1) or not (LOAD_ADDRESS <= entry_addr <
                              LOAD_ADDRESS + SLOT_SIZE):
        errors.append(f"reset vector 0x{entry:08x} is not inside the slot")
    return errors


def build_header(binary: bytes, args) -> bytes:
    version = semver(args.version)
    image_crc = zlib.crc32(binary) & 0xFFFFFFFF
    timestamp = int(args.timestamp, 0) if args.timestamp else 0
    entry = struct.unpack_from("<I", binary, 4)[0]

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<I", header, 0, MAGIC)
    struct.pack_into("<H", header, 4, HEADER_VERSION)
    struct.pack_into("<H", header, 6, HEADER_SIZE)
    struct.pack_into("<I", header, 8, IMAGE_TYPE_APPLICATION)
    struct.pack_into("<I", header, 12, version)
    struct.pack_into("<I", header, 16, len(binary))
    struct.pack_into("<I", header, 20, image_crc)
    struct.pack_into("<I", header, 24, LOAD_ADDRESS)
    struct.pack_into("<I", header, 28, entry)
    struct.pack_into("<I", header, 32, timestamp)
    header[36:56] = commit_bytes(args.commit)
    header[56:88] = args.name.encode("ascii", "replace")[:32].ljust(32, b"\0")
    struct.pack_into("<I", header, 252, zlib.crc32(header[:252]) & 0xFFFFFFFF)
    return bytes(header)


def read_image(path: pathlib.Path) -> tuple[bytes, bytes, int]:
    """Return (header, body, image_crc32) after validating an .img file."""
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{path}: shorter than a {HEADER_SIZE}-byte header")
    fields = IMAGE_HEADER.unpack_from(data, 0)
    (magic, header_version, header_size, image_type, _version, image_size,
     image_crc32, load_address, _entry, _timestamp, _git, _name,
     _reserved, header_crc32) = fields
    if magic != MAGIC:
        raise ValueError(f"{path}: bad image magic")
    if header_version != HEADER_VERSION or header_size != HEADER_SIZE or \
            image_type != IMAGE_TYPE_APPLICATION:
        raise ValueError(f"{path}: bad image header version, size or type")
    if load_address != LOAD_ADDRESS:
        raise ValueError(f"{path}: bad load address")
    if image_size > SLOT_SIZE:
        raise ValueError(f"{path}: image is larger than the slot")
    if zlib.crc32(data[:252]) & 0xFFFFFFFF != header_crc32:
        raise ValueError(f"{path}: header CRC32 mismatch")
    body = data[HEADER_SIZE:]
    if len(body) != image_size:
        raise ValueError(f"{path}: image_size does not match the body")
    if zlib.crc32(body) & 0xFFFFFFFF != image_crc32:
        raise ValueError(f"{path}: image CRC32 mismatch")
    return data[:HEADER_SIZE], body, image_crc32


def metadata_record(magic: int, value: int) -> bytes:
    record = bytearray(META_RECORD_SIZE)
    struct.pack_into("<II", record, 0, magic, value)
    struct.pack_into("<I", record, META_RECORD_SIZE - 4,
                     zlib.crc32(record[:META_RECORD_SIZE - 4]) & 0xFFFFFFFF)
    return bytes(record)


def build_metadata(image_header: bytes, image_crc32: int, sequence: int,
                   confirmed: bool = True) -> bytes:
    """Build one 16 KB Appendix B metadata sector (P1-4)."""
    if len(image_header) != HEADER_SIZE:
        raise ValueError("metadata needs a 256-byte image header")
    sector = bytearray(b"\xff" * META_SIZE)
    sector[0:HEADER_SIZE] = image_header
    sector[META_VERIFIED_OFFSET:META_VERIFIED_OFFSET + META_RECORD_SIZE] = \
        metadata_record(META_VERIFIED_MAGIC, image_crc32)
    sector[META_ACTIVATED_OFFSET:META_ACTIVATED_OFFSET + META_RECORD_SIZE] = \
        metadata_record(META_ACTIVATED_MAGIC, sequence)
    if confirmed:
        sector[META_CONFIRMED_OFFSET:
               META_CONFIRMED_OFFSET + META_RECORD_SIZE] = \
            metadata_record(META_CONFIRMED_MAGIC, 0)
    return bytes(sector)


def read_ihex(path: pathlib.Path) -> list[tuple[int, bytes]]:
    """Minimal Intel HEX reader for the BOOT image."""
    segments: list[tuple[int, bytes]] = []
    upper = 0
    chunk = bytearray()
    chunk_addr = 0
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"{path}:{number}: not an Intel HEX record")
        raw = bytes.fromhex(line[1:])
        if len(raw) < 5 or (sum(raw) & 0xFF) != 0:
            raise ValueError(f"{path}:{number}: bad Intel HEX checksum")
        length, address, kind = raw[0], (raw[1] << 8) | raw[2], raw[3]
        payload = raw[4:4 + length]
        if kind == 0x00:
            absolute = upper + address
            if chunk and absolute == chunk_addr + len(chunk):
                chunk.extend(payload)
            else:
                if chunk:
                    segments.append((chunk_addr, bytes(chunk)))
                chunk, chunk_addr = bytearray(payload), absolute
        elif kind == 0x01:
            break
        elif kind == 0x04:
            upper = ((payload[0] << 8) | payload[1]) << 16
        elif kind == 0x02:
            upper = ((payload[0] << 8) | payload[1]) << 4
        else:
            continue
    if chunk:
        segments.append((chunk_addr, bytes(chunk)))
    if not segments:
        raise ValueError(f"{path}: no data records")
    return segments


def write_ihex(path: pathlib.Path, segments: list[tuple[int, bytes]]) -> None:
    """Write sorted address/data segments as Intel HEX."""
    lines: list[str] = []
    current_upper: int | None = None

    def record(address: int, kind: int, payload: bytes) -> None:
        data = bytearray([len(payload), (address >> 8) & 0xFF,
                          address & 0xFF, kind]) + payload
        data.append((-sum(data)) & 0xFF)
        lines.append(":" + data.hex().upper())

    for address, data in sorted(segments, key=lambda item: item[0]):
        upper = address >> 16
        if upper != current_upper:
            record(0, 0x04, bytes([upper >> 8, upper & 0xFF]))
            current_upper = upper
        offset = 0
        while offset < len(data):
            take = min(16, len(data) - offset)
            record(address + offset, 0x00, data[offset:offset + take])
            offset += take
    record(0, 0x01, b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def factory_segments(boot: pathlib.Path, image_a: pathlib.Path,
                     image_b: pathlib.Path, seq_a: int,
                     seq_b: int) -> list[tuple[int, bytes]]:
    """BOOT + slot A/B + both metadata sectors at physical addresses."""
    if boot.suffix.lower() == ".hex":
        segments = read_ihex(boot)
    elif boot.suffix.lower() == ".bin":
        segments = [(BOOT_BASE, boot.read_bytes())]
    else:
        raise ValueError("--boot must be a .hex or .bin file")
    for address, data in segments:
        if address < BOOT_BASE or \
                address + len(data) > BOOT_BASE + BOOT_REGION_SIZE:
            raise ValueError(
                f"BOOT data at 0x{address:08x} exceeds the 32 KB region")

    for slot, image, sequence, slot_base, meta_base in (
            ("A", image_a, seq_a, SLOT_A_BASE, META_A_BASE),
            ("B", image_b, seq_b, SLOT_B_BASE, META_B_BASE)):
        header, body, image_crc = read_image(image)
        metadata = build_metadata(header, image_crc, sequence,
                                  confirmed=True)
        if len(body) > SLOT_SIZE:
            raise ValueError(f"slot {slot}: image exceeds the slot")
        segments.append((slot_base, body))
        segments.append((meta_base, metadata))
    return segments


def inspect(path: pathlib.Path) -> int:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        print(f"{path}: shorter than a 256-byte header", file=sys.stderr)
        return 1

    header = data[:HEADER_SIZE]
    fields = {}
    offset = 0
    for name, fmt in HEADER_FIELDS:
        fields[name], = struct.unpack_from(fmt, header, offset)
        offset += struct.calcsize(fmt)
    git_commit = header[36:56]
    name = header[56:88].rstrip(b"\0").decode("ascii", "replace")
    header_crc, = struct.unpack_from("<I", header, 252)
    body = data[HEADER_SIZE:]

    errors = []
    if fields["magic"] != MAGIC:
        errors.append("bad magic")
    if fields["header_version"] != HEADER_VERSION:
        errors.append("bad header version")
    if fields["header_size"] != HEADER_SIZE:
        errors.append("bad header size")
    if fields["image_type"] != IMAGE_TYPE_APPLICATION:
        errors.append("bad image type")
    if fields["load_address"] != LOAD_ADDRESS:
        errors.append("bad load address")
    if fields["image_size"] > SLOT_SIZE:
        errors.append("image size exceeds the slot")
    if fields["image_size"] != len(body):
        errors.append("image_size does not match the file body")
    if zlib.crc32(header[:252]) & 0xFFFFFFFF != header_crc:
        errors.append("header CRC32 mismatch")
    if zlib.crc32(body) & 0xFFFFFFFF != fields["image_crc32"]:
        errors.append("image CRC32 mismatch")
    errors.extend(validate_binary(body))

    print(f"file          : {path}")
    print(f"magic         : 0x{fields['magic']:08x}")
    print(f"header        : version {fields['header_version']}, "
          f"{fields['header_size']} bytes")
    print(f"image type    : {fields['image_type']}")
    print(f"version       : {version_text(fields['version'])}")
    print(f"image size    : {fields['image_size']} bytes")
    print(f"image CRC32   : 0x{fields['image_crc32']:08x}")
    print(f"load address  : 0x{fields['load_address']:08x}")
    print(f"entry address : 0x{fields['entry_address']:08x}")
    print(f"build time    : {fields['build_timestamp']}")
    print(f"git commit    : {commit_text(git_commit)}")
    print(f"name          : {name}")
    print(f"header CRC32  : 0x{header_crc:08x}")

    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--commit", default="")
    parser.add_argument("--timestamp", default="0")
    parser.add_argument("--name", default="stm32-application")
    parser.add_argument("--inspect", type=pathlib.Path,
                        help="print and check an existing image file")
    parser.add_argument("--metadata", choices=("A", "B"),
                        help="build the 16 KB metadata sector for a slot "
                             "from --image (P1-4)")
    parser.add_argument("--image", type=pathlib.Path,
                        help="validated .img used by --metadata")
    parser.add_argument("--sequence", type=int, default=None,
                        help="ACTIVATED sequence for --metadata "
                             "(default: A=1, B=2)")
    parser.add_argument("--no-confirmed", action="store_true",
                        help="leave the CONFIRMED record erased")
    parser.add_argument("--factory-hex", type=pathlib.Path, metavar="OUT",
                        help="write BOOT + both slots + both metadata "
                             "sectors as one Intel HEX file")
    parser.add_argument("--boot", type=pathlib.Path,
                        help=".hex or .bin BOOT image for --factory-hex")
    parser.add_argument("--slot-a", type=pathlib.Path,
                        help="slot A .img for --factory-hex")
    parser.add_argument("--slot-b", type=pathlib.Path,
                        help="slot B .img for --factory-hex")
    parser.add_argument("--seq-a", type=int, default=1)
    parser.add_argument("--seq-b", type=int, default=2)
    args = parser.parse_args()

    if args.inspect is not None:
        return inspect(args.inspect)

    if args.factory_hex is not None:
        if not (args.boot and args.slot_a and args.slot_b):
            parser.error("--factory-hex needs --boot, --slot-a and --slot-b")
        try:
            segments = factory_segments(args.boot, args.slot_a, args.slot_b,
                                        args.seq_a, args.seq_b)
            write_ihex(args.factory_hex, segments)
        except (OSError, ValueError) as error:
            print(f"mkimage: {error}", file=sys.stderr)
            return 1
        total = sum(len(data) for _address, data in segments)
        print(f"mkimage: {args.factory_hex} (factory image, "
              f"{total} bytes in {len(segments)} segments)")
        return 0

    if args.metadata is not None:
        if args.image is None or args.output is None:
            parser.error("--metadata needs --image and --output")
        try:
            header, _body, image_crc = read_image(args.image)
            sequence = args.sequence
            if sequence is None:
                sequence = 1 if args.metadata == "A" else 2
            sector = build_metadata(header, image_crc, sequence,
                                    not args.no_confirmed)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            if args.output.suffix.lower() == ".hex":
                base = META_A_BASE if args.metadata == "A" else META_B_BASE
                write_ihex(args.output, [(base, sector)])
            else:
                args.output.write_bytes(sector)
        except (OSError, ValueError) as error:
            print(f"mkimage: {error}", file=sys.stderr)
            return 1
        print(f"mkimage: {args.output} (metadata slot {args.metadata}, "
              f"sequence {sequence}, {len(sector)} bytes)")
        return 0

    if args.input is None or args.output is None:
        parser.error("--input and --output are required unless --inspect")

    binary = args.input.read_bytes()
    errors = validate_binary(binary)
    if errors:
        for error in errors:
            print(f"mkimage: {error}", file=sys.stderr)
        return 1

    header = build_header(binary, args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + binary)

    image_crc = zlib.crc32(binary) & 0xFFFFFFFF
    print(f"mkimage: {args.output} ({HEADER_SIZE + len(binary)} bytes, "
          f"version {args.version}, crc 0x{image_crc:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
