#!/usr/bin/env python3
#
# bootctl.py - command-line updater for the A/B bootloader
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  NUCLEO-F767ZI over the ST-LINK VCP
# SRS:     BHOST-004; SRS-BOOT-F767 section 9 and Appendix A
#
# Usage:
#   tools/bootctl.py info
#   tools/bootctl.py update build/app.img --activate
#   tools/bootctl.py activate
#   tools/bootctl.py reset
#   tools/bootctl.py set-baud 921600
#
# The protocol code is generated from protocol/hwlink.fbs by flatc and the
# Python flatbuffers runtime is vendored under tools/vendor.  Only pyserial
# is needed on the host:  pip install pyserial

import argparse
import glob
import pathlib
import struct
import sys
import time
import zlib

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS / "generated"))
sys.path.insert(0, str(TOOLS / "vendor"))

import flatbuffers  # noqa: E402
from hwlink import Ack, Activate, BootInfo, BootOutcome, Error, \
    GetBootInfo, Hello, HelloReply, ImageHeader, Message, Nak, Progress, \
    Reset, Role, SetBaud, Slot, UpdateAbort, UpdateBegin, UpdateData, \
    UpdateEnd, UpdateResult  # noqa: E402

PROTOCOL_MAJOR = 1
PROTOCOL_MINOR = 1
HWLINK_FLAG_RESPONSE = 1 << 0
HWLINK_FLAG_EVENT = 1 << 1
MAX_FRAME = 2048
HEADER_SIZE = 256
LOAD_ADDRESS = 0x0800C000
SLOT_SIZE = 0x000F4000
IMAGE_MAGIC = 0x48574249

IMAGE_HEADER = struct.Struct("<IHHIIIIIII20s32s164sI")


def error_name(value: int) -> str:
    names = {
        Error.NONE: "NONE",
        Error.MALFORMED: "MALFORMED",
        Error.UNSUPPORTED: "UNSUPPORTED",
        Error.BAD_ARGUMENT: "BAD_ARGUMENT",
        Error.BAD_STATE: "BAD_STATE",
        Error.BUSY: "BUSY",
        Error.NOT_FOUND: "NOT_FOUND",
        Error.BAD_HEADER: "BAD_HEADER",
        Error.BAD_LAYOUT: "BAD_LAYOUT",
        Error.BAD_VECTORS: "BAD_VECTORS",
        Error.OUT_OF_ORDER: "OUT_OF_ORDER",
        Error.FLASH_ERROR: "FLASH_ERROR",
        Error.CRC_MISMATCH: "CRC_MISMATCH",
        Error.WRONG_FLASH_MODE: "WRONG_FLASH_MODE",
    }
    return names.get(value, f"ERROR_{value}")


def slot_name(value: int) -> str:
    return {Slot.A: "A", Slot.B: "B"}.get(value, "?")


def outcome_name(value: int) -> str:
    return {
        BootOutcome.BOOTED: "BOOTED",
        BootOutcome.ROLLED_BACK: "ROLLED_BACK",
        BootOutcome.UPDATE_MODE: "UPDATE_MODE",
    }.get(value, "UNKNOWN")


def text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("ascii", "replace")
    return str(value)


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    out.append(0)
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_index] = 0xFF
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    out.append(0)
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        index += 1
        if code == 0:
            raise ValueError("unexpected COBS delimiter")
        block = code - 1
        if index + block > len(data):
            raise ValueError("truncated COBS block")
        out.extend(data[index:index + block])
        index += block
        if code != 0xFF and index < len(data):
            out.append(0)
    return bytes(out)


def build_raw(payload: bytes, seq: int, flags: int = 0) -> bytes:
    raw = bytes([PROTOCOL_MAJOR, flags]) + struct.pack("<H", seq) + payload
    return raw + struct.pack("<I", zlib.crc32(raw) & 0xFFFFFFFF)


def decode_raw(raw: bytes):
    if len(raw) < 8 or len(raw) > MAX_FRAME or raw[0] != PROTOCOL_MAJOR:
        return None
    body_len = len(raw) - 4
    crc, = struct.unpack_from("<I", raw, body_len)
    if zlib.crc32(raw[:body_len]) & 0xFFFFFFFF != crc:
        return None
    flags = raw[1]
    seq, = struct.unpack_from("<H", raw, 2)
    return flags, seq, raw[4:body_len]


def finish(builder: flatbuffers.Builder, body_type: int, body) -> bytes:
    Message.MessageStart(builder)
    Message.MessageAddBodyType(builder, body_type)
    Message.MessageAddBody(builder, body)
    message = Message.MessageEnd(builder)
    builder.Finish(message, file_identifier=b"HWLK")
    return bytes(builder.Output())


def as_table(body, cls):
    table = cls()
    table.Init(body.Bytes, body.Pos)
    return table


class LinkError(RuntimeError):
    pass


class Link:
    def __init__(self, port: str, baud: int, timeout: float):
        import serial
        self.port = port
        self.timeout = timeout
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=2)
        self.seq = 0
        self.buffer = bytearray()

    def close(self):
        if self.ser is not None and self.ser.is_open:
            self.ser.close()

    def _next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFFFF
        if self.seq == 0:
            self.seq = 1
        return self.seq

    def _send(self, payload: bytes, seq: int):
        self.ser.write(cobs_encode(build_raw(payload, seq)))
        self.ser.flush()

    def _read_frame(self, deadline: float):
        while time.monotonic() < deadline:
            byte = self.ser.read(1)
            if not byte:
                continue
            if byte[0] == 0:
                if not self.buffer:
                    continue
                encoded = bytes(self.buffer)
                self.buffer.clear()
                try:
                    frame = decode_raw(cobs_decode(encoded))
                except ValueError:
                    continue
                if frame is not None:
                    return frame
            elif len(self.buffer) < MAX_FRAME + 64:
                self.buffer.append(byte[0])
            else:
                self.buffer.clear()
        return None

    def _handle_event(self, payload: bytes):
        message = Message.Message.GetRootAs(payload, 0)
        if message.BodyType() != 9:   # Progress
            return
        progress = Progress.Progress()
        progress.Init(message.Body().Bytes, message.Body().Pos)
        done = progress.Done()
        total = progress.Total()
        percent = (100 * done / total) if total else 0
        print(f"  progress {text(progress.Operation())}: "
              f"{done}/{total} ({percent:.0f}%)", flush=True)

    def request(self, payload: bytes, timeout: float | None = None,
                seq: int | None = None) -> bytes:
        if timeout is None:
            timeout = self.timeout
        if seq is None:
            seq = self._next_seq()
        deadline = time.monotonic() + timeout
        self._send(payload, seq)
        while time.monotonic() < deadline:
            frame = self._read_frame(deadline)
            if frame is None:
                break
            flags, frame_seq, response = frame
            if flags & HWLINK_FLAG_EVENT:
                self._handle_event(response)
                continue
            if not (flags & HWLINK_FLAG_RESPONSE) or frame_seq != seq:
                continue
            return response
        raise LinkError(f"timeout waiting for reply to sequence {seq}")

    def hello(self) -> HelloReply:
        builder = flatbuffers.Builder(256)
        host = builder.CreateString("bootctl")
        version = builder.CreateString("0.1.0")
        Hello.HelloStart(builder)
        Hello.HelloAddHostName(builder, host)
        Hello.HelloAddHostVersion(builder, version)
        Hello.HelloAddProtocolMinor(builder, PROTOCOL_MINOR)
        body = Hello.HelloEnd(builder)
        response = self.request(finish(builder, 1, body))
        message = Message.Message.GetRootAs(response, 0)
        if message.BodyType() != 2:   # HelloReply
            raise LinkError("device did not answer Hello with HelloReply")
        reply = as_table(message.Body(), HelloReply.HelloReply)
        if reply.Role() != Role.BOOTLOADER:
            raise LinkError(f"device role is {reply.Role()}, not BOOTLOADER")
        return reply


def parse_image(path: pathlib.Path):
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise LinkError(f"{path}: shorter than a {HEADER_SIZE}-byte header")
    fields = IMAGE_HEADER.unpack_from(data, 0)
    (magic, header_version, header_size, image_type, version, image_size,
     image_crc32, load_address, entry_address, build_timestamp, git_commit,
     name, _reserved, header_crc32) = fields
    if magic != IMAGE_MAGIC:
        raise LinkError("image: bad magic")
    if header_version != 1 or header_size != HEADER_SIZE or image_type != 1:
        raise LinkError("image: unsupported header version, size or type")
    if load_address != LOAD_ADDRESS:
        raise LinkError("image: wrong load address")
    if image_size > SLOT_SIZE:
        raise LinkError("image: larger than the 976 KB slot")
    if zlib.crc32(data[:252]) & 0xFFFFFFFF != header_crc32:
        raise LinkError("image: header CRC32 mismatch")
    body = data[HEADER_SIZE:]
    if len(body) != image_size:
        raise LinkError("image: image_size does not match the file body")
    if zlib.crc32(body) & 0xFFFFFFFF != image_crc32:
        raise LinkError("image: image CRC32 mismatch")
    return {
        "version": version,
        "image_size": image_size,
        "image_crc32": image_crc32,
        "header_crc32": header_crc32,
        "load_address": load_address,
        "entry_address": entry_address,
        "build_timestamp": build_timestamp,
        "git_commit": git_commit,
        "name": name,
        "body": body,
    }


def cmd_info(link: Link, _args) -> int:
    reply = link.hello()
    print(f"firmware     : {text(reply.FirmwareName())} "
          f"{text(reply.Version())} ({text(reply.Commit())})")
    print(f"build        : {text(reply.BuildTime())}")
    print(f"flash mode   : {'DUAL_BANK' if reply.FlashMode() == 2 else 'SINGLE_BANK'}")
    print(f"detail       : {text(reply.Detail())}")

    builder = flatbuffers.Builder(64)
    GetBootInfo.GetBootInfoStart(builder)
    body = GetBootInfo.GetBootInfoEnd(builder)
    response = link.request(finish(builder, 11, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() != 12:   # BootInfo
        raise LinkError("device did not answer GetBootInfo with BootInfo")
    info = as_table(message.Body(), BootInfo.BootInfo)
    print(f"active slot  : {slot_name(info.ActiveSlot())}")
    print(f"last outcome : {outcome_name(info.LastOutcome())}")
    print(f"rejected slot: {slot_name(info.RejectedSlot())}")
    for index in range(info.SlotsLength()):
        slot = info.Slots(index)
        print(f"slot {slot_name(slot.Slot())}: "
              f"present={slot.Present()} valid={slot.Valid()} "
              f"pending={slot.Pending()} confirmed={slot.Confirmed()} "
              f"rejected={slot.Rejected()} seq={slot.Sequence()} "
              f"trial={slot.TrialCount()}")
        print(f"  version {((slot.Version() >> 24) & 0xFF)}."
              f"{((slot.Version() >> 16) & 0xFF)}."
              f"{slot.Version() & 0xFF}, size {slot.ImageSize()}, "
              f"build {slot.BuildTimestamp()}")
        print(f"  name {text(slot.Name())} commit {text(slot.GitCommit())}")
        print(f"  detail: {text(slot.Detail())}")
    return 0


def send_update_begin(link: Link, image) -> None:
    builder = flatbuffers.Builder(1024)
    git = builder.CreateByteVector(image["git_commit"])
    name = builder.CreateString(
        image["name"].rstrip(b"\0").decode("ascii", "replace"))
    ImageHeader.ImageHeaderStart(builder)
    ImageHeader.ImageHeaderAddMagic(builder, IMAGE_MAGIC)
    ImageHeader.ImageHeaderAddHeaderVersion(builder, 1)
    ImageHeader.ImageHeaderAddHeaderSize(builder, HEADER_SIZE)
    ImageHeader.ImageHeaderAddImageType(builder, 1)
    ImageHeader.ImageHeaderAddVersion(builder, image["version"])
    ImageHeader.ImageHeaderAddImageSize(builder, image["image_size"])
    ImageHeader.ImageHeaderAddImageCrc32(builder, image["image_crc32"])
    ImageHeader.ImageHeaderAddLoadAddress(builder, image["load_address"])
    ImageHeader.ImageHeaderAddEntryAddress(builder, image["entry_address"])
    ImageHeader.ImageHeaderAddBuildTimestamp(builder, image["build_timestamp"])
    ImageHeader.ImageHeaderAddGitCommit(builder, git)
    ImageHeader.ImageHeaderAddName(builder, name)
    ImageHeader.ImageHeaderAddHeaderCrc32(builder, image["header_crc32"])
    header = ImageHeader.ImageHeaderEnd(builder)

    UpdateBegin.UpdateBeginStart(builder)
    UpdateBegin.UpdateBeginAddHeader(builder, header)
    body = UpdateBegin.UpdateBeginEnd(builder)
    response = link.request(finish(builder, 13, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() == 7:   # Nak
        nak = as_table(message.Body(), Nak.Nak)
        raise LinkError(f"UpdateBegin: {error_name(nak.Error())}: "
                        f"{text(nak.Detail())}")


def send_update_data(link: Link, offset: int, chunk: bytes) -> int:
    builder = flatbuffers.Builder(len(chunk) + 256)
    data = builder.CreateByteVector(chunk)
    UpdateData.UpdateDataStart(builder)
    UpdateData.UpdateDataAddOffset(builder, offset)
    UpdateData.UpdateDataAddData(builder, data)
    body = UpdateData.UpdateDataEnd(builder)
    response = link.request(finish(builder, 14, body))
    message = Message.Message.GetRootAs(response, 0)

    if message.BodyType() == 6:   # Ack
        ack = as_table(message.Body(), Ack.Ack)
        return ack.NextOffset()

    if message.BodyType() == 7:   # Nak
        nak = as_table(message.Body(), Nak.Nak)
        if nak.Error() == Error.OUT_OF_ORDER:
            print(f"  resync: expected offset {nak.ExpectedOffset()}",
                  flush=True)
            return nak.ExpectedOffset()
        raise LinkError(f"UpdateData at {offset}: "
                        f"{error_name(nak.Error())}: {text(nak.Detail())}")

    raise LinkError("UpdateData: unexpected reply")


def send_update_abort(link: Link) -> None:
    try:
        builder = flatbuffers.Builder(32)
        UpdateAbort.UpdateAbortStart(builder)
        body = UpdateAbort.UpdateAbortEnd(builder)
        link.request(finish(builder, 18, body))
    except (LinkError, OSError):
        pass


def cmd_update(link: Link, args) -> int:
    image = parse_image(pathlib.Path(args.image))
    print(f"image: {args.image} version "
          f"{((image['version'] >> 24) & 0xFF)}."
          f"{((image['version'] >> 16) & 0xFF)}.{image['version'] & 0xFF}, "
          f"{image['image_size']} bytes, crc "
          f"0x{image['image_crc32']:08x}")
    link.hello()

    try:
        send_update_begin(link, image)
        print("UpdateBegin: ok")

        body = image["body"]
        offset = 0
        last_report = 0
        while offset < len(body):
            chunk = body[offset:offset + args.chunk]
            next_offset = send_update_data(link, offset, chunk)
            if next_offset == offset:
                raise LinkError(f"device did not advance from offset {offset}")
            offset = next_offset
            percent = (100 * offset / len(body)) if body else 100
            if percent >= last_report + 10 or offset == len(body):
                print(f"  written {offset}/{len(body)} ({percent:.0f}%)",
                      flush=True)
                last_report = percent

        builder = flatbuffers.Builder(32)
        UpdateEnd.UpdateEndStart(builder)
        body_end = UpdateEnd.UpdateEndEnd(builder)
        response = link.request(finish(builder, 15, body_end),
                                timeout=max(link.timeout, 10.0))
        message = Message.Message.GetRootAs(response, 0)
        if message.BodyType() != 16:   # UpdateResult
            raise LinkError("UpdateEnd: unexpected reply")
        result = as_table(message.Body(), UpdateResult.UpdateResult)
        if not result.Success():
            raise LinkError(f"UpdateEnd: {error_name(result.Error())}: "
                            f"{text(result.Detail())}")
        print(f"UpdateEnd: verified, CRC 0x{result.ImageCrc32():08x}")
    except LinkError:
        send_update_abort(link)
        raise

    if args.activate:
        cmd_activate(link, args)
    return 0


def cmd_activate(link: Link, _args) -> int:
    link.hello()
    builder = flatbuffers.Builder(32)
    Activate.ActivateStart(builder)
    body = Activate.ActivateEnd(builder)
    response = link.request(finish(builder, 17, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() == 7:
        nak = as_table(message.Body(), Nak.Nak)
        raise LinkError(f"Activate: {error_name(nak.Error())}: "
                        f"{text(nak.Detail())}")
    print("Activate: ok, device is resetting")
    return 0


def cmd_reset(link: Link, _args) -> int:
    link.hello()
    builder = flatbuffers.Builder(32)
    Reset.ResetStart(builder)
    body = Reset.ResetEnd(builder)
    link.request(finish(builder, 10, body))
    print("Reset: ok")
    return 0


def cmd_set_baud(link: Link, args) -> int:
    link.hello()
    builder = flatbuffers.Builder(64)
    SetBaud.SetBaudStart(builder)
    SetBaud.SetBaudAddRate(builder, args.rate)
    body = SetBaud.SetBaudEnd(builder)
    response = link.request(finish(builder, 5, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() == 7:
        nak = as_table(message.Body(), Nak.Nak)
        raise LinkError(f"SetBaud: {error_name(nak.Error())}: "
                        f"{text(nak.Detail())}")
    time.sleep(0.1)
    link.ser.baudrate = args.rate
    link.ser.reset_input_buffer()
    print(f"SetBaud: {args.rate}")
    return 0


def find_port() -> str:
    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    if not candidates:
        raise LinkError("no /dev/ttyACM* or /dev/ttyUSB* port found")
    if len(candidates) > 1:
        raise LinkError("several serial ports found; use --port: " +
                        ", ".join(candidates))
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None,
                        help="serial port (default: first ttyACM/ttyUSB)")
    parser.add_argument("--baud", type=int, default=115200,
                        choices=(115200, 921600))
    parser.add_argument("--timeout", type=float, default=10.0)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="show HelloReply and BootInfo")
    update = sub.add_parser("update", help="write and verify an image")
    update.add_argument("image")
    update.add_argument("--chunk", type=int, default=1024)
    update.add_argument("--activate", action="store_true",
                        help="activate and reset after a successful verify")
    sub.add_parser("activate", help="activate the verified slot and reset")
    sub.add_parser("reset", help="leave BOOT and boot normally")
    set_baud = sub.add_parser("set-baud", help="switch the link baud rate")
    set_baud.add_argument("rate", type=int, choices=(115200, 921600))

    args = parser.parse_args()
    if args.command == "update" and (args.chunk < 1 or args.chunk > 1024):
        parser.error("--chunk must be 1..1024")

    try:
        port = args.port or find_port()
        link = Link(port, args.baud, args.timeout)
    except ImportError:
        print("bootctl: pyserial is required: pip install pyserial",
              file=sys.stderr)
        return 1
    except (LinkError, OSError) as error:
        print(f"bootctl: {error}", file=sys.stderr)
        return 1

    commands = {
        "info": cmd_info,
        "update": cmd_update,
        "activate": cmd_activate,
        "reset": cmd_reset,
        "set-baud": cmd_set_baud,
    }

    try:
        return commands[args.command](link, args)
    except (LinkError, OSError) as error:
        print(f"bootctl: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
