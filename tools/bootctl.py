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
#   tools/bootctl.py info [--json]
#   tools/bootctl.py update build/app.img --activate --wait
#   tools/bootctl.py update --both --a a.img --b b.img --json
#   tools/bootctl.py activate [--no-wait]
#   tools/bootctl.py enter | wait | reset | set-baud 921600
#
# Every command accepts --json (one JSON object per line).  If the
# application is running, bootctl sends EnterBootloader and reopens the
# port unless --no-enter is given.
#
# The protocol code is generated from protocol/hwlink.fbs by flatc and the
# Python flatbuffers runtime is vendored under tools/vendor.  Only pyserial
# is needed on the host:  pip install pyserial

import argparse
import glob
import hashlib
import json
import pathlib
import struct
import sys
import time
import zlib

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS / "generated"))
sys.path.insert(0, str(TOOLS / "vendor"))

import flatbuffers  # noqa: E402
from hwlink import Ack, Activate, BootInfo, EnterBootloader, GetBootInfo, \
    GetStatus, Hello, HelloReply, ImageHeader, Message, Nak, Progress, Reset, \
    SetBaud, Status, UpdateAbort, UpdateBegin, UpdateData, UpdateEnd, \
    UpdateResult  # noqa: E402
# Enums live in modules of the same name; import the classes themselves.
from hwlink.BootOutcome import BootOutcome  # noqa: E402
from hwlink.Error import Error  # noqa: E402
from hwlink.HealthState import HealthState  # noqa: E402
from hwlink.Role import Role  # noqa: E402
from hwlink.Slot import Slot  # noqa: E402
from hwlink.TrialState import TrialState  # noqa: E402

PROTOCOL_MAJOR = 1
PROTOCOL_MINOR = 3
HWLINK_FLAG_RESPONSE = 1 << 0
HWLINK_FLAG_EVENT = 1 << 1
MAX_FRAME = 2048
HEADER_SIZE = 256
ACTIVATE_TIMEOUT = 5.0
VERBOSE = False
JSON_OUT = False
LOAD_ADDRESS = 0x0800C000
SLOT_SIZE = 0x000F4000
IMAGE_MAGIC = 0x48574249

IMAGE_HEADER = struct.Struct("<IHHIIIIIII20s32s164sI")


def vprint(*args) -> None:
    """Print only with --verbose."""
    if VERBOSE:
        print(*args, flush=True)


def emit(obj, **fields) -> None:
    """One JSON object per line for --json (P2-7)."""
    if not JSON_OUT:
        return
    payload = dict(fields)
    payload.update(obj)
    print(json.dumps(payload, separators=(",", ":")), flush=True)


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
        Error.AUTH_FAILED: "AUTH_FAILED",
        Error.ROLLBACK: "ROLLBACK",
    }
    return names.get(value, f"ERROR_{value}")


def slot_name(value: int) -> str:
    return {Slot.A: "A", Slot.B: "B"}.get(value, "?")


def version_text(value: int) -> str:
    return f"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{value & 0xFF}"


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
    def __init__(self, port: str, baud: int, timeout: float,
                 auto_enter: bool = True, json_output: bool = False):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.auto_enter = auto_enter
        self.json_output = json_output
        self.ser = None
        self.seq = 0
        self.buffer = bytearray()
        self.verbose = False
        self.open()

    def open(self):
        import serial
        if self.ser is not None and self.ser.is_open:
            self.ser.close()
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05,
                                 write_timeout=2)
        self.buffer.clear()
        return self

    def reopen(self):
        return self.open()

    def close(self):
        if self.ser is not None and self.ser.is_open:
            self.ser.close()

    def _next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFFFF
        if self.seq == 0:
            self.seq = 1
        return self.seq

    def _send(self, payload: bytes, seq: int):
        frame = cobs_encode(build_raw(payload, seq))
        self.ser.write(frame)
        self.ser.flush()
        if self.verbose:
            print(f"  tx seq {seq:5d}  payload {len(payload):4d} B  "
                  f"frame {len(frame):4d} B", flush=True)

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
        operation = text(progress.Operation())
        percent = (100 * done / total) if total else 0
        if self.json_output:
            emit({"event": "progress", "operation": operation,
                  "done": done, "total": total, "percent": percent})
            return
        if operation == "erase":
            print(f"  erasing slot sector {done}/{total}", flush=True)
        else:
            print(f"  progress {operation}: {done}/{total} "
                  f"({percent:.0f}%)", flush=True)

    def request(self, payload: bytes, timeout: float | None = None,
                seq: int | None = None, retries: int = 1) -> bytes:
        if timeout is None:
            timeout = self.timeout
        if seq is None:
            seq = self._next_seq()
        # Resend the same sequence number on timeout: BOOT replays the
        # cached reply instead of running the request twice (BNFR-003).
        for attempt in range(retries + 1):
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
                if self.verbose:
                    print(f"  rx seq {frame_seq:5d}  payload "
                          f"{len(response):4d} B", flush=True)
                return response
            if attempt == retries:
                break
        raise LinkError(f"timeout waiting for reply to sequence {seq}")

    def _hello_once(self, timeout: float | None = None) -> HelloReply:
        builder = flatbuffers.Builder(256)
        host = builder.CreateString("bootctl")
        version = builder.CreateString("0.2.0")
        Hello.HelloStart(builder)
        Hello.HelloAddHostName(builder, host)
        Hello.HelloAddHostVersion(builder, version)
        Hello.HelloAddProtocolMinor(builder, PROTOCOL_MINOR)
        body = Hello.HelloEnd(builder)
        response = self.request(finish(builder, 1, body), timeout=timeout)
        message = Message.Message.GetRootAs(response, 0)
        if message.BodyType() != 2:   # HelloReply
            raise LinkError("device did not answer Hello with HelloReply")
        return as_table(message.Body(), HelloReply.HelloReply)

    def _enter_bootloader_request(self) -> None:
        builder = flatbuffers.Builder(16)
        EnterBootloader.EnterBootloaderStart(builder)
        body = EnterBootloader.EnterBootloaderEnd(builder)
        response = self.request(finish(builder, 22, body), timeout=2.0)
        message = Message.Message.GetRootAs(response, 0)
        if message.BodyType() == 7:  # Nak
            nak = as_table(message.Body(), Nak.Nak)
            raise LinkError(f"EnterBootloader: {error_name(nak.Error())}: "
                            f"{text(nak.Detail())}")

    def hello(self, require_boot: bool = True,
              timeout: float | None = None) -> HelloReply:
        """Hello with P1-1 auto-enter.

        A running application answers Hello with role APPLICATION.  Unless
        require_boot is false, send EnterBootloader, reopen the port and
        retry Hello until BOOT answers.
        """
        reply = self._hello_once(timeout)
        if reply.Role() == Role.BOOTLOADER or not require_boot:
            return reply
        if not self.auto_enter:
            raise LinkError(
                "device is running the application; use 'bootctl enter' or "
                "do not pass --no-enter")

        vprint(f"hello        : role {reply.Role()} (application); "
               f"sending EnterBootloader")
        last_error: Exception | None = None
        try:
            self._enter_bootloader_request()
        except LinkError as error:
            # The application may reset before its Ack reaches us.  Treat a
            # lost reply as success-pending and confirm by retrying Hello.
            last_error = error
        self.close()
        time.sleep(0.25)

        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            try:
                self.open()
                remaining = max(0.5, min(1.5, deadline - time.monotonic()))
                reply = self._hello_once(timeout=remaining)
                if reply.Role() == Role.BOOTLOADER:
                    return reply
            except (LinkError, OSError) as error:
                last_error = error
            self.close()
            time.sleep(0.2)
        raise LinkError(f"could not enter BOOT: {last_error}")


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
    # BSAFE-006: version 2 is signed, version 1 unsigned.  BOOT decides
    # whether an unsigned image is acceptable.
    if header_version not in (1, 2) or header_size != HEADER_SIZE or \
            image_type != 1:
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
    security_counter, key_id = struct.unpack_from("<II", data, 88)
    if header_version == 2 and hashlib.sha256(body).digest() != data[96:128]:
        raise LinkError("image: image SHA-256 mismatch")
    return {
        "header_version": header_version,
        # Header bytes 88..251, sent verbatim (protocol minor 3).
        "extension": data[88:252],
        "security_counter": security_counter,
        "key_id": key_id,
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


def boot_info_dict(reply, info) -> dict:
    """Machine-readable form of HelloReply + BootInfo (info --json)."""
    slots = []
    for index in range(info.SlotsLength()):
        slot = info.Slots(index)
        slots.append({
            "slot": slot_name(slot.Slot()),
            "present": bool(slot.Present()),
            "valid": bool(slot.Valid()),
            "pending": bool(slot.Pending()),
            "confirmed": bool(slot.Confirmed()),
            "rejected": bool(slot.Rejected()),
            "sequence": slot.Sequence(),
            "trial_count": slot.TrialCount(),
            "version": version_text(slot.Version()),
            "image_size": slot.ImageSize(),
            "build_timestamp": slot.BuildTimestamp(),
            "git_commit": text(slot.GitCommit()),
            "name": text(slot.Name()),
            "detail": text(slot.Detail()),
            "security_counter": slot.SecurityCounter(),
        })
    return {
        "firmware": text(reply.FirmwareName()),
        "version": text(reply.Version()),
        "commit": text(reply.Commit()),
        "build_time": text(reply.BuildTime()),
        "flash_mode": "DUAL_BANK" if reply.FlashMode() == 2 else "SINGLE_BANK",
        "detail": text(reply.Detail()),
        "active_slot": slot_name(info.ActiveSlot()),
        "active_valid": bool(info.ActiveValid()),
        "target_slot": slot_name(info.TargetSlot()),
        "boot_version": text(info.BootVersion()),
        "boot_build": text(info.BootBuild()),
        "last_outcome": outcome_name(info.LastOutcome()),
        "rejected_slot": slot_name(info.RejectedSlot()),
        "signed_images": bool(info.SignedImages()),
        "key_id": f"0x{info.KeyId():08x}",
        "security_floor": info.SecurityFloor(),
        "slots": slots,
    }


def read_boot_info(link: Link):
    builder = flatbuffers.Builder(64)
    GetBootInfo.GetBootInfoStart(builder)
    body = GetBootInfo.GetBootInfoEnd(builder)
    response = link.request(finish(builder, 11, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() != 12:   # BootInfo
        raise LinkError("device did not answer GetBootInfo with BootInfo")
    return as_table(message.Body(), BootInfo.BootInfo)


def read_status(link: Link):
    """GetStatus from a running application (used by --both / --wait)."""
    builder = flatbuffers.Builder(32)
    GetStatus.GetStatusStart(builder)
    body = GetStatus.GetStatusEnd(builder)
    response = link.request(finish(builder, 23, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() != 24:   # Status
        raise LinkError("device did not answer GetStatus with Status")
    return as_table(message.Body(), Status.Status)


def trial_name(value: int) -> str:
    return {
        TrialState.CONFIRMED: "CONFIRMED",
        TrialState.PENDING: "PENDING",
    }.get(value, "UNKNOWN")


def health_name(value: int) -> str:
    return {
        HealthState.NOT_REQUIRED: "NOT_REQUIRED",
        HealthState.RUNNING: "RUNNING",
        HealthState.PASSED: "PASSED",
        HealthState.FAILED: "FAILED",
    }.get(value, "UNKNOWN")


def enter_boot_from_application(port: str, baud: int) -> None:
    """P1-1: ask a running application to reset into BOOT."""
    link = Link(port, baud, 2.0, auto_enter=False)
    try:
        reply = link.hello(require_boot=False, timeout=2.0)
        if reply.Role() == Role.BOOTLOADER:
            return
        link._enter_bootloader_request()
    finally:
        link.close()
    time.sleep(0.3)


def open_boot_link(port: str, baud: int, timeout: float) -> Link:
    """Open BOOT, entering it from the application when necessary."""
    for attempt in range(3):
        link = None
        try:
            link = Link(port, baud, timeout, auto_enter=True)
            reply = link.hello()
            if reply.Role() == Role.BOOTLOADER:
                return link
        except (LinkError, OSError):
            if link is not None:
                link.close()
            if attempt == 2:
                raise
            time.sleep(0.5)
    raise LinkError("could not open BOOT")


# After a Reset, stay off the port until BOOT's 500 ms listen window and its
# image checks (about 1.2 s for a full slot) are over.
BOOT_SETTLE_S = 2.0
# How often wait may release BOOT from its listen window before giving up.
MAX_BOOT_RELEASES = 3


def wait_for_confirmation(port: str, baud: int, timeout: float,
                          expected_slot: str | None = None) -> dict:
    """P2-3: wait until the application confirms the freshly activated slot.

    Returns a status dict.  A rollback is reported when BOOT boots the other
    slot instead or when the application reports a different slot/failed
    health.  The application must implement GetStatus (the BOOT contract
    already defines it)."""
    deadline = time.monotonic() + timeout
    last = "no reply"
    releases = 0
    while time.monotonic() < deadline:
        link = None
        try:
            link = Link(port, baud, 1.0, auto_enter=False)
            reply = link.hello(require_boot=False, timeout=1.0)
            if reply.Role() == Role.BOOTLOADER:
                # The board reset back into BOOT: usually a rollback or a
                # failed image.  Read the boot info to report it.
                info = read_boot_info(link)
                data = boot_info_dict(reply, info)
                rolled = data["last_outcome"] == "ROLLED_BACK"
                last = f"still in BOOT ({data['last_outcome']})"
                # last_outcome stays ROLLED_BACK until the next successful
                # update (TRY-008), so it proves a rollback only together
                # with a different active slot, or with nothing bootable.
                if rolled and expected_slot in ("A", "B") and \
                        data["active_slot"] != expected_slot:
                    return {"state": "rolled_back", **data}
                if not data["active_valid"]:
                    if rolled:
                        return {"state": "rolled_back", **data}
                elif releases < MAX_BOOT_RELEASES:
                    # A bootable slot and BOOT answers anyway: this Hello
                    # reached BOOT inside its 500 ms listen window after
                    # the reset (ENT-001), which keeps BOOT in update mode
                    # before the trial starts.  Let it boot; no trial
                    # attempt was used.
                    releases += 1
                    vprint("wait: caught BOOT's listen window; resetting "
                           "so the application boots")
                    send_reset(link)
                    link.close()
                    link = None
                    time.sleep(BOOT_SETTLE_S)
                    continue
            else:
                status = read_status(link)
                slot = slot_name(status.Slot())
                trial = trial_name(status.Trial())
                health = health_name(status.Health())
                data = {
                    "state": "confirmed" if trial == "CONFIRMED" else
                             "pending",
                    "slot": slot,
                    "trial": trial,
                    "trial_count": status.TrialCount(),
                    "health": health,
                    "version": text(status.Version()),
                    "commit": text(status.Commit()),
                }
                if expected_slot in ("A", "B") and slot != expected_slot:
                    data["state"] = "rolled_back"
                    return data
                if health == "FAILED":
                    data["state"] = "failed"
                    return data
                if data["state"] == "confirmed":
                    return data
                last = f"slot {slot} {trial}, health {health}"
        except (LinkError, OSError) as error:
            last = str(error)
        finally:
            if link is not None:
                link.close()
        time.sleep(0.5)
    raise LinkError(f"application did not confirm the trial: {last}")


def cmd_info(link: Link, args) -> int:
    reply = link.hello()
    info = read_boot_info(link)

    if getattr(args, "json", False):
        print(json.dumps(boot_info_dict(reply, info), indent=2))
        return 0

    print(f"firmware     : {text(reply.FirmwareName())} "
          f"{text(reply.Version())} ({text(reply.Commit())})")
    print(f"build        : {text(reply.BuildTime())}")
    print(f"flash mode   : {'DUAL_BANK' if reply.FlashMode() == 2 else 'SINGLE_BANK'}")
    print(f"detail       : {text(reply.Detail())}")
    print(f"active slot  : {slot_name(info.ActiveSlot())}"
          f"{' (none)' if not info.ActiveValid() else ''}")
    print(f"target slot  : {slot_name(info.TargetSlot())} (BOOT decides)")
    print(f"boot build   : {text(info.BootVersion())} "
          f"({text(info.BootBuild())})")
    print(f"last outcome : {outcome_name(info.LastOutcome())}")
    print(f"rejected slot: {slot_name(info.RejectedSlot())}")
    if info.SignedImages():
        print(f"signing      : required, key id 0x{info.KeyId():08x}, "
              f"security floor {info.SecurityFloor()}")
    else:
        print("signing      : not required (or BOOT older than minor 3)")
    for index in range(info.SlotsLength()):
        slot = info.Slots(index)
        print(f"slot {slot_name(slot.Slot())}: "
              f"present={slot.Present()} valid={slot.Valid()} "
              f"pending={slot.Pending()} confirmed={slot.Confirmed()} "
              f"rejected={slot.Rejected()} seq={slot.Sequence()} "
              f"trial={slot.TrialCount()}")
        print(f"  version {version_text(slot.Version())}, "
              f"size {slot.ImageSize()}, build {slot.BuildTimestamp()}, "
              f"security counter {slot.SecurityCounter()}")
        print(f"  name {text(slot.Name())} commit {text(slot.GitCommit())}")
        print(f"  detail: {text(slot.Detail())}")
    return 0


def send_update_begin(link: Link, image, force: bool = False) -> None:
    builder = flatbuffers.Builder(1024)
    git = builder.CreateByteVector(image["git_commit"])
    name = builder.CreateString(
        image["name"].rstrip(b"\0").decode("ascii", "replace"))
    extension = builder.CreateByteVector(image.get("extension", bytes(164)))
    ImageHeader.ImageHeaderStart(builder)
    ImageHeader.ImageHeaderAddMagic(builder, IMAGE_MAGIC)
    ImageHeader.ImageHeaderAddHeaderVersion(builder,
                                            image.get("header_version", 1))
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
    ImageHeader.ImageHeaderAddExtension(builder, extension)
    header = ImageHeader.ImageHeaderEnd(builder)

    UpdateBegin.UpdateBeginStart(builder)
    UpdateBegin.UpdateBeginAddHeader(builder, header)
    UpdateBegin.UpdateBeginAddForce(builder, force)
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


SLOT_BASE = {"A": 0x0800C000, "B": 0x0810C000}


def slot_base(slot: str | None) -> int:
    return SLOT_BASE.get(slot or "A", SLOT_BASE["A"])


def print_image(image, path: str) -> None:
    print(f"image        : {path} version "
          f"{version_text(image['version'])}, "
          f"{image['image_size']} bytes, crc "
          f"0x{image['image_crc32']:08x}")
    if not VERBOSE:
        return
    name = image["name"].rstrip(b"\0").decode("ascii", "replace")
    stamp = time.strftime("%Y-%m-%d %H:%M:%S",
                          time.gmtime(image["build_timestamp"]))
    vprint(f"  header       : 256 B, load 0x{image['load_address']:08x}, "
           f"entry 0x{image['entry_address']:08x}")
    vprint(f"  build time   : {stamp}")
    vprint(f"  name         : {name}")
    vprint(f"  git commit   : {image['git_commit'].hex()}")
    vprint(f"  header crc32 : 0x{image['header_crc32']:08x}")
    if image["header_version"] == 2:
        vprint(f"  signed       : key id 0x{image['key_id']:08x}, "
               f"security counter {image['security_counter']}")
    else:
        vprint("  signed       : no (version 1 header)")
    vprint(f"  body         : {len(image['body'])} B")


def print_boot_target(link: Link) -> str | None:
    info = read_boot_info(link)
    active = slot_name(info.ActiveSlot())
    target = slot_name(info.TargetSlot())
    if target not in ("A", "B"):
        target = "A" if not info.ActiveValid() else (
            "B" if active == "A" else "A")
    vprint(f"boot info    : active {active} "
           f"({'valid' if info.ActiveValid() else 'none'}), "
           f"target {target} @ 0x{slot_base(target):08x}")
    for index in range(info.SlotsLength()):
        slot = info.Slots(index)
        vprint(f"  slot {slot_name(slot.Slot())}: present={slot.Present()} "
               f"valid={slot.Valid()} pending={slot.Pending()} "
               f"confirmed={slot.Confirmed()} rejected={slot.Rejected()} "
               f"seq={slot.Sequence()}")
    return target


def upload_image(link: Link, image, args, target: str | None) -> dict:
    """Send UpdateBegin/Data/End and return the verified result."""
    started = time.monotonic()
    try:
        send_update_begin(link, image, force=getattr(args, "force", False))
        if not JSON_OUT:
            print("UpdateBegin: ok")
        emit({"event": "update_begin", "image_size": image["image_size"],
              "image_crc32": image["image_crc32"], "target_slot": target})
        vprint(f"  header written to the metadata sector; "
               f"{image['image_size']} bytes to flash")

        body = image["body"]
        offset = 0
        last_report = 0
        while offset < len(body):
            chunk = body[offset:offset + args.chunk]
            next_offset = send_update_data(link, offset, chunk)
            if next_offset == offset:
                raise LinkError(f"device did not advance from offset {offset}")
            if VERBOSE:
                elapsed = max(time.monotonic() - started, 1e-6)
                rate = offset / elapsed / 1024
                preview = " ".join(f"{b:02x}" for b in chunk[:12])
                if len(chunk) > 12:
                    preview += " ..."
                address = (f"@0x{slot_base(target) + offset:08x} "
                           if target else "")
                vprint(f"  {offset:08x}  {len(chunk):4d} B {address}"
                       f"-> {next_offset:08x}  "
                       f"{100 * next_offset / len(body):6.1f}%  "
                       f"{rate:7.1f} KB/s  {preview}")
            offset = next_offset
            percent = (100 * offset / len(body)) if body else 100
            if percent >= last_report + 10 or offset == len(body):
                if JSON_OUT:
                    emit({"event": "progress", "operation": "upload",
                          "done": offset, "total": len(body),
                          "percent": percent})
                elif not VERBOSE:
                    print(f"  written {offset}/{len(body)} "
                          f"({percent:.0f}%)", flush=True)
                last_report = percent

        builder = flatbuffers.Builder(32)
        UpdateEnd.UpdateEndStart(builder)
        body_end = UpdateEnd.UpdateEndEnd(builder)
        verify_start = time.monotonic()
        response = link.request(finish(builder, 15, body_end),
                                timeout=max(link.timeout, 10.0))
        verify_time = time.monotonic() - verify_start
        message = Message.Message.GetRootAs(response, 0)
        if message.BodyType() != 16:   # UpdateResult
            raise LinkError("UpdateEnd: unexpected reply")
        result = as_table(message.Body(), UpdateResult.UpdateResult)
        if not result.Success():
            raise LinkError(f"UpdateEnd: {error_name(result.Error())}: "
                            f"{text(result.Detail())}")
        if not JSON_OUT:
            print(f"UpdateEnd: verified, CRC 0x{result.ImageCrc32():08x}")
        emit({"event": "update_end", "status": "verified",
              "image_crc32": result.ImageCrc32()})
        if VERBOSE:
            total = time.monotonic() - started
            rate = image["image_size"] / total / 1024 if total else 0
            vprint(f"  verified {image['image_size']} bytes in "
                   f"{verify_time:.2f} s; flash+verify {total:.2f} s, "
                   f"{rate:.1f} KB/s")
        return {"image": image, "crc": result.ImageCrc32(),
                "seconds": time.monotonic() - started}
    except LinkError:
        send_update_abort(link)
        raise


def activate_slot(link: Link, expected_slot: str | None = None) -> str:
    """Activate and reset.  Returns "ok", or "pending" when the Ack was
    lost (P2-5); callers then confirm with GetStatus/GetBootInfo."""
    builder = flatbuffers.Builder(32)
    Activate.ActivateStart(builder)
    body = Activate.ActivateEnd(builder)
    try:
        response = link.request(finish(builder, 17, body),
                                timeout=max(link.timeout,
                                            ACTIVATE_TIMEOUT))
    except LinkError as error:
        if not JSON_OUT:
            print("Activate: reply lost; checking the new slot instead")
        emit({"event": "activate", "status": "pending",
              "reason": str(error), "slot": expected_slot})
        return "pending"
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() == 7:
        nak = as_table(message.Body(), Nak.Nak)
        raise LinkError(f"Activate: {error_name(nak.Error())}: "
                        f"{text(nak.Detail())}")
    if not JSON_OUT:
        print("Activate: ok, device is resetting")
    emit({"event": "activate", "status": "ok", "slot": expected_slot})
    return "ok"


def _image_for_slot(args, slot: str):
    """Pick the image for a slot from --a/--b or the positional image."""
    a_path = getattr(args, "a", None)
    b_path = getattr(args, "b", None)
    if a_path and b_path:
        return a_path if slot == "A" else b_path
    if a_path or b_path:
        raise LinkError("--a and --b must be given together")
    return args.image


def cmd_update_both(link: Link, args) -> int:
    """P1-2: update/activate the inactive slot, wait for CONFIRMED, then
    enter BOOT and do the same for the other slot."""
    if not args.image and not (args.a and args.b):
        raise LinkError("update --both needs IMAGE or --a IMG --b IMG")

    port, baud = link.port, link.baud
    link.close()
    boot = open_boot_link(port, baud, args.timeout)
    try:
        reply = boot.hello()
        info = read_boot_info(boot)
        target = slot_name(info.TargetSlot())
        if target not in ("A", "B"):
            target = "A" if not info.ActiveValid() else (
                "B" if slot_name(info.ActiveSlot()) == "A" else "A")
    finally:
        boot.close()

    stages = []
    for index, slot in enumerate((target, "B" if target == "A" else "A")):
        if index > 0:
            enter_boot_from_application(port, baud)
        path = _image_for_slot(args, slot)
        image = parse_image(pathlib.Path(path))
        if not JSON_OUT:
            print(f"== slot {slot}: {path} ==")
        emit({"event": "stage", "stage": slot, "status": "updating",
              "image": str(path)})
        boot = open_boot_link(port, baud, args.timeout)
        try:
            reply = boot.hello()
            info = read_boot_info(boot)
            actual = slot_name(info.TargetSlot())
            if actual in ("A", "B") and actual != slot:
                raise LinkError(f"BOOT targets slot {actual}, not {slot}")
            if getattr(args, "fast", False):
                change_baud(boot, 921600)
            upload_image(boot, image, args, slot)
            activate_slot(boot, slot)
        finally:
            boot.close()
        confirmation = wait_for_confirmation(port, baud,
                                             args.wait_timeout, slot)
        if confirmation.get("state") in ("rolled_back", "failed"):
            emit({"event": "stage", "stage": slot,
                  "status": confirmation.get("state"), **confirmation})
            raise LinkError(f"slot {slot} did not confirm "
                            f"({confirmation.get('state')})")
        stages.append({"slot": slot, "image": str(path),
                       "status": "confirmed", **confirmation})
        if not JSON_OUT:
            print(f"slot {slot}: confirmed by the application")
        emit({"event": "stage", "stage": slot, "status": "confirmed",
              **confirmation})

    enter_boot_from_application(port, baud)
    final = open_boot_link(port, baud, args.timeout)
    try:
        final_reply = final.hello()
        final_info = read_boot_info(final)
        final_dict = boot_info_dict(final_reply, final_info)
        if not JSON_OUT:
            for index in range(final_info.SlotsLength()):
                slot = final_info.Slots(index)
                print(f"slot {slot_name(slot.Slot())}: valid={slot.Valid()} "
                      f"confirmed={slot.Confirmed()} "
                      f"seq={slot.Sequence()}")
        emit({"event": "result", "command": "update --both",
              "status": "ok", "stages": stages, "info": final_dict})
    finally:
        final.close()
    return 0


def cmd_update(link: Link, args) -> int:
    if getattr(args, "both", False) or getattr(args, "a", None) or \
            getattr(args, "b", None):
        return cmd_update_both(link, args)
    if not args.image:
        raise LinkError("update needs an image")

    image = parse_image(pathlib.Path(args.image))
    if not JSON_OUT:
        print_image(image, args.image)
    reply = link.hello()
    if VERBOSE:
        mode = "DUAL_BANK" if reply.FlashMode() == 2 else "SINGLE_BANK"
        vprint(f"hello        : {text(reply.FirmwareName())} "
               f"{text(reply.Version())} ({text(reply.Commit())}), "
               f"protocol {reply.ProtocolMajor()}.{reply.ProtocolMinor()}, "
               f"flash mode {mode}")
    target = None
    try:
        info = read_boot_info(link)
        target = slot_name(info.TargetSlot())
        if target not in ("A", "B"):
            target = "A" if not info.ActiveValid() else (
                "B" if slot_name(info.ActiveSlot()) == "A" else "A")
        if VERBOSE:
            print_boot_target(link)
        emit({"event": "boot_info", "active_slot":
              slot_name(info.ActiveSlot()), "active_valid":
              bool(info.ActiveValid()), "target_slot": target})
    except LinkError as error:
        vprint(f"boot info    : unavailable ({error})")

    if getattr(args, "fast", False):
        change_baud(link, 921600)

    result = upload_image(link, image, args, target)

    if args.activate:
        state = activate_slot(link, target)
        if state == "pending" or getattr(args, "wait", False):
            link.close()
            confirmation = wait_for_confirmation(
                link.port, link.baud, args.wait_timeout, target)
            if not JSON_OUT:
                print(f"application: slot {confirmation.get('slot')} "
                      f"{confirmation.get('trial')} "
                      f"(health {confirmation.get('health')})")
            emit({"event": "confirm", **confirmation})
            if confirmation.get("state") in ("rolled_back", "failed"):
                raise LinkError(
                    f"the new image did not confirm "
                    f"({confirmation.get('state')})")
    emit({"event": "result", "command": "update", "status": "ok",
          "target_slot": target, "image": args.image,
          "image_crc32": result["crc"],
          "seconds": round(result["seconds"], 3)})
    return 0


def cmd_enter(link: Link, _args) -> int:
    """Ask a running application to reset into the bootloader."""
    reply = link.hello(require_boot=False)
    if reply.Role() == Role.BOOTLOADER:
        if not JSON_OUT:
            print("EnterBootloader: already in BOOT")
        emit({"event": "enter", "status": "already-in-boot"})
        return 0
    link._enter_bootloader_request()
    if not JSON_OUT:
        print("EnterBootloader: ok, the application is resetting into BOOT")
    emit({"event": "enter", "status": "ok"})
    return 0


def cmd_activate(link: Link, args) -> int:
    reply = link.hello()
    info = read_boot_info(link)
    target = slot_name(info.TargetSlot())
    state = activate_slot(link, target)
    if state == "pending" or getattr(args, "wait", True):
        link.close()
        confirmation = wait_for_confirmation(
            link.port, link.baud, getattr(args, "wait_timeout", 30.0),
            target)
        if not JSON_OUT:
            print(f"application: slot {confirmation.get('slot')} "
                  f"{confirmation.get('trial')}")
        emit({"event": "confirm", **confirmation})
        if confirmation.get("state") in ("rolled_back", "failed"):
            raise LinkError(f"the activated image did not confirm "
                            f"({confirmation.get('state')})")
    emit({"event": "result", "command": "activate", "status": "ok",
          "target_slot": target})
    return 0


def send_reset(link: Link) -> None:
    builder = flatbuffers.Builder(32)
    Reset.ResetStart(builder)
    body = Reset.ResetEnd(builder)
    link.request(finish(builder, 10, body))


def cmd_reset(link: Link, _args) -> int:
    link.hello()
    send_reset(link)
    if not JSON_OUT:
        print("Reset: ok")
    emit({"event": "result", "command": "reset", "status": "ok"})
    return 0


# catch: Hello is resent this often, so one lands inside BOOT's 500 ms
# listen window (ENT-001).
CATCH_INTERVAL_S = 0.05


def cmd_catch(link: Link, args) -> int:
    """Keep sending Hello until BOOT answers, then show the boot info.

    For an application that does not answer HWLINK: start this, then reset
    the board (SWD, NRST or the reset button).  A Hello inside BOOT's
    listen window keeps BOOT in update mode.
    """
    deadline = time.monotonic() + args.catch_timeout
    while time.monotonic() < deadline:
        try:
            reply = link._hello_once(CATCH_INTERVAL_S)
        except LinkError:
            continue
        if reply.Role() == Role.BOOTLOADER:
            info = read_boot_info(link)
            if JSON_OUT:
                print(json.dumps(boot_info_dict(reply, info), indent=2))
            else:
                print(f"caught BOOT: active slot {slot_name(info.ActiveSlot())}"
                      f", last outcome {outcome_name(info.LastOutcome())}")
            return 0
    raise LinkError(f"BOOT did not answer within {args.catch_timeout:g} s; "
                    "reset the board while catch is running")


def cmd_wait(link: Link, args) -> int:
    """Wait for the running application to confirm its trial slot."""
    port, baud = link.port, link.baud
    link.close()
    confirmation = wait_for_confirmation(port, baud, args.wait_timeout)
    if not JSON_OUT:
        print(f"application: slot {confirmation.get('slot')} "
              f"{confirmation.get('trial')}")
    if confirmation.get("state") in ("rolled_back", "failed"):
        emit({"event": "result", "command": "wait",
              "status": confirmation.get("state"), **confirmation})
        raise LinkError(f"application did not confirm "
                        f"({confirmation.get('state')})")
    emit({"event": "result", "command": "wait", "status": "ok",
          **confirmation})
    return 0


def change_baud(link: Link, rate: int) -> None:
    """Send SetBaud and switch the host port inside the same session."""
    builder = flatbuffers.Builder(64)
    SetBaud.SetBaudStart(builder)
    SetBaud.SetBaudAddRate(builder, rate)
    body = SetBaud.SetBaudEnd(builder)
    response = link.request(finish(builder, 5, body))
    message = Message.Message.GetRootAs(response, 0)
    if message.BodyType() == 7:
        nak = as_table(message.Body(), Nak.Nak)
        raise LinkError(f"SetBaud: {error_name(nak.Error())}: "
                        f"{text(nak.Detail())}")
    time.sleep(0.1)
    link.baud = rate
    link.ser.baudrate = rate
    link.ser.reset_input_buffer()
    if not JSON_OUT:
        print(f"SetBaud: {rate}")
    emit({"event": "set_baud", "rate": rate})


def cmd_set_baud(link: Link, args) -> int:
    link.hello()
    change_baud(link, args.rate)
    return 0


def list_stlink_ports() -> list[str]:
    """P2-5: ST-LINK VCP ports (VID 0x0483) on Linux, macOS and Windows."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))

    ports = list(list_ports.comports())
    stlink = sorted(port.device for port in ports
                    if getattr(port, "vid", None) == 0x0483)
    if stlink:
        return stlink
    return sorted(port.device for port in ports)


def find_port() -> str:
    candidates = list_stlink_ports()
    if not candidates:
        raise LinkError("no ST-LINK serial port found")
    if len(candidates) > 1:
        raise LinkError("several ST-LINK serial ports found; use --port: " +
                        ", ".join(candidates))
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None,
                        help="serial port (default: first ST-LINK VCP)")
    parser.add_argument("--baud", type=int, default=115200,
                        choices=(115200, 921600))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--no-enter", action="store_true",
                        help="do not send EnterBootloader when an "
                             "application is running (P1-1)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="log every chunk, offset, flash address and frame")
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="show HelloReply and BootInfo")
    info.add_argument("--json", action="store_true",
                      help="print machine-readable JSON")
    update = sub.add_parser("update", help="write and verify an image")
    update.add_argument("image", nargs="?")
    update.add_argument("--chunk", type=int, default=1024)
    update.add_argument("--activate", action="store_true",
                        help="activate and reset after a successful verify")
    update.add_argument("--wait", action="store_true",
                        help="wait for the application to confirm the trial")
    update.add_argument("--wait-timeout", type=float, default=30.0,
                        help="seconds to wait for CONFIRMED (30)")
    update.add_argument("--fast", action="store_true",
                        help="switch to 921600 baud for the upload")
    update.add_argument("--force", action="store_true",
                        help="bench only: allow an update while the active "
                             "slot is on trial")
    update.add_argument("--both", action="store_true",
                        help="update + activate both slots and verify the "
                             "application confirms each one")
    update.add_argument("--a", metavar="IMG", default=None,
                        help="image for slot A (with --b and --both)")
    update.add_argument("--b", metavar="IMG", default=None,
                        help="image for slot B (with --a and --both)")
    update.add_argument("--json", action="store_true",
                        help="print one JSON object per line")

    activate = sub.add_parser("activate",
                              help="activate the verified slot and reset")
    activate.add_argument("--no-wait", dest="wait", action="store_false",
                          default=True,
                          help="do not wait for the application to confirm")
    activate.add_argument("--wait-timeout", type=float, default=30.0)
    activate.add_argument("--json", action="store_true")
    enter = sub.add_parser("enter",
                           help="ask a running application to enter BOOT")
    enter.add_argument("--json", action="store_true")
    wait = sub.add_parser("wait",
                          help="wait for the application to confirm its trial")
    wait.add_argument("--wait-timeout", type=float, default=30.0)
    wait.add_argument("--json", action="store_true")
    catch = sub.add_parser(
        "catch", help="send Hello until BOOT answers; reset the board "
                      "meanwhile (for an application without HWLINK)")
    catch.add_argument("--for", dest="catch_timeout", type=float,
                       default=15.0, help="seconds to keep trying (15)")
    catch.add_argument("--json", action="store_true")
    reset = sub.add_parser("reset", help="leave BOOT and boot normally")
    reset.add_argument("--json", action="store_true")
    set_baud = sub.add_parser("set-baud", help="switch the link baud rate")
    set_baud.add_argument("rate", type=int, choices=(115200, 921600))
    set_baud.add_argument("--json", action="store_true")

    args = parser.parse_args()
    if args.command == "update" and (args.chunk < 1 or args.chunk > 1024):
        parser.error("--chunk must be 1..1024")
    if args.command == "update" and not args.image and \
            not (args.a and args.b):
        parser.error("update needs IMAGE or --a IMG --b IMG")
    if args.command == "update" and bool(args.a) != bool(args.b):
        parser.error("--a and --b must be given together")

    global VERBOSE, JSON_OUT
    JSON_OUT = bool(getattr(args, "json", False))
    # Keep --json output machine-readable even if -v is also given.
    VERBOSE = args.verbose and not JSON_OUT

    link = None
    try:
        port = args.port or find_port()
        link = Link(port, args.baud, args.timeout,
                    auto_enter=not args.no_enter, json_output=JSON_OUT)
        link.verbose = VERBOSE
    except ImportError:
        print("bootctl: pyserial is required: pip install pyserial",
              file=sys.stderr)
        return 1
    except (LinkError, OSError) as error:
        if JSON_OUT:
            emit({"event": "error", "command": args.command,
                  "status": "error", "error": str(error)})
        else:
            print(f"bootctl: {error}", file=sys.stderr)
        return 1

    commands = {
        "info": cmd_info,
        "update": cmd_update,
        "activate": cmd_activate,
        "enter": cmd_enter,
        "wait": cmd_wait,
        "catch": cmd_catch,
        "reset": cmd_reset,
        "set-baud": cmd_set_baud,
    }

    try:
        return commands[args.command](link, args)
    except (LinkError, OSError) as error:
        if JSON_OUT:
            emit({"event": "error", "command": args.command,
                  "status": "error", "error": str(error)})
        else:
            print(f"bootctl: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if link is not None:
            link.close()


if __name__ == "__main__":
    sys.exit(main())
