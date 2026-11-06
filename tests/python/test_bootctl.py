#!/usr/bin/env python3
"""
Python tests for bootctl.py and mkimage.py.

The protocol test runs a tiny simulated BOOT on a pty and drives the real
Link/code with the same HWLINK framing and FlatBuffers messages.  It covers
the P1-2 --both loop, EnterBootloader auto-enter and the P1-4 metadata
layout.  BSAFE-006: mkimage signing, inspection and re-signing, and the
signed header reaching BOOT intact through UpdateBegin.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import pty
import select
import struct
import sys
import threading
import time
import tty

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "generated"))
sys.path.insert(0, str(TOOLS / "vendor"))

import flatbuffers  # noqa: E402
import bootctl  # noqa: E402
import mkimage  # noqa: E402
import package_hwlink  # noqa: E402
import check_contracts  # noqa: E402
from hwlink import Ack, BootInfo, BootSlotInfo, EnterBootloader, GetBootInfo, GetStatus, \
    HelloReply, Message, Nak, Status, UpdateBegin, UpdateData, UpdateEnd, \
    UpdateResult  # noqa: E402
from hwlink.Error import Error  # noqa: E402
from hwlink.HealthState import HealthState  # noqa: E402
from hwlink.Role import Role  # noqa: E402
from hwlink.Slot import Slot  # noqa: E402
from hwlink.TrialState import TrialState  # noqa: E402

RESPONSE = bootctl.HWLINK_FLAG_RESPONSE


def _finish(builder, body_type, body):
    return bootctl.finish(builder, body_type, body)


class FakeBoot(threading.Thread):
    """A minimal BOOT + application state machine on a pty."""

    def __init__(self) -> None:
        super().__init__(daemon=True)
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.slave_name = os.ttyname(slave)
        self.stop = threading.Event()
        self.role = Role.BOOTLOADER
        self.active = "A"
        self.target = "B"
        self.last_seq = None
        self.last_reply = None
        self.buffer = bytearray()
        self.drop_next_type: int | None = None
        self.drop_all_types: set[int] = set()
        self.expect_offset = 0
        self.begin_header: bytes | None = None
        self.active_pending = False   # report the active slot on trial
        self.resets = 0

    def run(self) -> None:
        while not self.stop.is_set():
            ready, _, _ = select.select([self.master], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError:
                break
            for byte in data:
                if byte == 0:
                    if self.buffer:
                        self._frame(bytes(self.buffer))
                    self.buffer.clear()
                else:
                    self.buffer.append(byte)

    def _send(self, payload: bytes, seq: int) -> None:
        frame = bootctl.cobs_encode(bootctl.build_raw(payload, seq, RESPONSE))
        os.write(self.master, frame)

    def _frame(self, encoded: bytes) -> None:
        raw = bootctl.cobs_decode(encoded)
        decoded = bootctl.decode_raw(raw)
        if decoded is None:
            return
        flags, seq, payload = decoded
        if flags & bootctl.HWLINK_FLAG_RESPONSE:
            return
        message = Message.Message.GetRootAs(payload, 0)
        body_type = message.BodyType()
        if body_type in self.drop_all_types:
            self.last_seq = seq
            self.last_reply = self._handle(payload, seq)
            return
        if self.last_seq == seq and self.last_reply is not None:
            os.write(self.master, self.last_reply)
            return
        self.last_seq = seq
        reply = self._handle(payload, seq)
        self.last_reply = reply
        if self.drop_next_type == body_type:
            self.drop_next_type = None
            return
        os.write(self.master, reply)

    def _hello_reply(self, seq: int) -> bytes:
        b = flatbuffers.Builder(256)
        name = b.CreateString("fake-boot")
        ver = b.CreateString("0.2.0")
        commit = b.CreateString("test")
        build = b.CreateString("now")
        detail = b.CreateString("simulated")
        HelloReply.HelloReplyStart(b)
        HelloReply.HelloReplyAddRole(b, self.role)
        HelloReply.HelloReplyAddFirmwareName(b, name)
        HelloReply.HelloReplyAddVersion(b, ver)
        HelloReply.HelloReplyAddCommit(b, commit)
        HelloReply.HelloReplyAddBuildTime(b, build)
        HelloReply.HelloReplyAddFlashMode(b, 2)
        HelloReply.HelloReplyAddProtocolMajor(b, 1)
        HelloReply.HelloReplyAddProtocolMinor(b, 2)
        HelloReply.HelloReplyAddMaxFrame(b, 2048)
        HelloReply.HelloReplyAddDetail(b, detail)
        body = HelloReply.HelloReplyEnd(b)
        return bootctl.cobs_encode(
            bootctl.build_raw(_finish(b, 2, body), seq, RESPONSE))

    def _boot_info(self, seq: int) -> bytes:
        b = flatbuffers.Builder(256)
        ver = b.CreateString("0.2.0")
        build = b.CreateString("now")
        slots = None
        if self.active_pending:
            BootSlotInfo.BootSlotInfoStart(b)
            BootSlotInfo.BootSlotInfoAddSlot(
                b, Slot.A if self.active == "A" else Slot.B)
            BootSlotInfo.BootSlotInfoAddPresent(b, True)
            BootSlotInfo.BootSlotInfoAddValid(b, True)
            BootSlotInfo.BootSlotInfoAddPending(b, True)
            slot = BootSlotInfo.BootSlotInfoEnd(b)
            BootInfo.BootInfoStartSlotsVector(b, 1)
            b.PrependUOffsetTRelative(slot)
            slots = b.EndVector()
        BootInfo.BootInfoStart(b)
        if slots is not None:
            BootInfo.BootInfoAddSlots(b, slots)
        BootInfo.BootInfoAddActiveSlot(
            b, Slot.A if self.active == "A" else Slot.B)
        BootInfo.BootInfoAddLastOutcome(b, 1)
        BootInfo.BootInfoAddRejectedSlot(b, 0)
        BootInfo.BootInfoAddFlashMode(b, 2)
        BootInfo.BootInfoAddActiveValid(b, True)
        BootInfo.BootInfoAddTargetSlot(
            b, Slot.A if self.target == "A" else Slot.B)
        BootInfo.BootInfoAddBootVersion(b, ver)
        BootInfo.BootInfoAddBootBuild(b, build)
        body = BootInfo.BootInfoEnd(b)
        return bootctl.cobs_encode(
            bootctl.build_raw(_finish(b, 12, body), seq, RESPONSE))

    def _status(self, seq: int) -> bytes:
        b = flatbuffers.Builder(256)
        ver = b.CreateString("0.1.0")
        commit = b.CreateString("test")
        Status.StatusStart(b)
        Status.StatusAddVersion(b, ver)
        Status.StatusAddCommit(b, commit)
        Status.StatusAddSlot(b, Slot.A if self.active == "A" else Slot.B)
        Status.StatusAddTrial(b, TrialState.CONFIRMED)
        Status.StatusAddTrialCount(b, 0)
        Status.StatusAddHealth(b, HealthState.PASSED)
        body = Status.StatusEnd(b)
        return bootctl.cobs_encode(
            bootctl.build_raw(_finish(b, 24, body), seq, RESPONSE))

    def _nak(self, seq: int, error: int, detail: str,
             expected_offset: int = 0) -> bytes:
        b = flatbuffers.Builder(128)
        text = b.CreateString(detail)
        Nak.NakStart(b)
        Nak.NakAddSeq(b, seq)
        Nak.NakAddError(b, error)
        Nak.NakAddDetail(b, text)
        Nak.NakAddExpectedOffset(b, expected_offset)
        body = Nak.NakEnd(b)
        return bootctl.cobs_encode(
            bootctl.build_raw(_finish(b, 7, body), seq, RESPONSE))

    @staticmethod
    def _rebuild_header(image) -> bytes:
        """The 256-byte header BOOT rebuilds from hwlink.ImageHeader."""
        import zlib
        header = bytearray(256)
        struct.pack_into("<IHHIIIIIII", header, 0, image.Magic(),
                         image.HeaderVersion(), image.HeaderSize(),
                         image.ImageType(), image.Version(),
                         image.ImageSize(), image.ImageCrc32(),
                         image.LoadAddress(), image.EntryAddress(),
                         image.BuildTimestamp())
        commit = bytes(image.GitCommit(i)
                       for i in range(image.GitCommitLength()))
        header[36:56] = commit[:20].ljust(20, b"\0")
        header[56:88] = (image.Name() or b"")[:32].ljust(32, b"\0")
        if image.ExtensionLength():
            header[88:252] = bytes(image.Extension(i)
                                   for i in range(image.ExtensionLength()))
        struct.pack_into("<I", header, 252, zlib.crc32(header[:252]))
        return bytes(header)

    def _ack(self, seq: int, next_offset: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        Ack.AckStart(b)
        Ack.AckAddSeq(b, seq)
        Ack.AckAddNextOffset(b, next_offset)
        body = Ack.AckEnd(b)
        return bootctl.cobs_encode(
            bootctl.build_raw(_finish(b, 6, body), seq, RESPONSE))

    def _handle(self, payload: bytes, seq: int) -> bytes:
        message = Message.Message.GetRootAs(payload, 0)
        body_type = message.BodyType()
        body = message.Body()

        if body_type == 1:      # Hello
            return self._hello_reply(seq)
        if body_type == 11:     # GetBootInfo
            if self.role == Role.BOOTLOADER:
                return self._boot_info(seq)
            return self._ack(seq)
        if body_type == 13:     # UpdateBegin
            begin = UpdateBegin.UpdateBegin()
            begin.Init(body.Bytes, body.Pos)
            self.begin_header = self._rebuild_header(begin.Header())
            self.expect_offset = 0
            return self._ack(seq, 0)
        if body_type == 14:     # UpdateData
            data = UpdateData.UpdateData()
            data.Init(body.Bytes, body.Pos)
            if data.Offset() != self.expect_offset:
                return self._nak(seq, Error.OUT_OF_ORDER,
                                 "offset does not match",
                                 self.expect_offset)
            self.expect_offset += data.DataLength()
            return self._ack(seq, self.expect_offset)
        if body_type == 15:     # UpdateEnd
            b = flatbuffers.Builder(64)
            UpdateResult.UpdateResultStart(b)
            UpdateResult.UpdateResultAddSuccess(b, True)
            UpdateResult.UpdateResultAddImageCrc32(b, 0)
            UpdateResult.UpdateResultAddError(b, Error.NONE)
            result = UpdateResult.UpdateResultEnd(b)
            return bootctl.cobs_encode(
                bootctl.build_raw(_finish(b, 16, result), seq, RESPONSE))
        if body_type == 17:     # Activate
            reply = self._ack(seq)
            if self.role == Role.BOOTLOADER:
                self.role = 3        # application/demo role
                self.active, self.target = self.target, self.active
            return reply
        if body_type == 22:     # EnterBootloader
            reply = self._ack(seq)
            self.role = Role.BOOTLOADER
            return reply
        if body_type == 23:     # GetStatus
            return self._status(seq)
        if body_type == 10:     # Reset: BOOT boots the active slot
            self.resets += 1
            if self.role == Role.BOOTLOADER and self.active_pending:
                self.role = 3
                self.active_pending = False
            return self._ack(seq)
        return self._ack(seq)


def _make_image(tmp_path: pathlib.Path, name: str = "app.img",
                extra: list[str] | None = None) -> pathlib.Path:
    binary = tmp_path / "app.bin"
    binary.write_bytes(struct.pack("<II", 0x20020000, 0x0800C001) +
                       bytes(range(40)))
    image = tmp_path / name
    argv = ["--input", str(binary), "--output", str(image),
            "--version", "1.2.3", "--commit", "0" * 40, "--name", "test",
            *(extra or [])]
    old = sys.argv
    sys.argv = ["mkimage.py", *argv]
    try:
        assert mkimage.main() == 0
    finally:
        sys.argv = old
    return image


def test_cobs_roundtrip():
    payload = bytes(range(256)) * 3
    encoded = bootctl.cobs_encode(payload)
    assert bootctl.cobs_decode(encoded[:-1]) == payload


def test_metadata_layout(tmp_path):
    image = _make_image(tmp_path)
    header, body, crc = mkimage.read_image(image)
    sector = mkimage.build_metadata(header, crc, 7)
    assert len(sector) == 0x4000
    assert sector[0:4] == b"IBWH"
    for offset, magic in ((0x100, 0x56455249), (0x120, 0x41435456),
                          (0x140, 0x434F4E46)):
        record = sector[offset:offset + 32]
        assert struct.unpack_from("<I", record, 0)[0] == magic
        import zlib
        assert zlib.crc32(record[:28]) & 0xFFFFFFFF == \
            struct.unpack_from("<I", record, 28)[0]


def test_auto_enter_from_application():
    device = FakeBoot()
    device.role = 3          # application/demo role
    device.start()
    try:
        link = bootctl.Link(device.slave_name, 115200, 2.0,
                            auto_enter=True)
        try:
            reply = link.hello()
        finally:
            link.close()
        assert reply.Role() == Role.BOOTLOADER
        assert device.role == Role.BOOTLOADER
    finally:
        device.stop.set()
        device.join(timeout=2)


def test_lost_reply_is_replayed_and_out_of_order_resyncs():
    device = FakeBoot()
    device.start()
    try:
        link = bootctl.Link(device.slave_name, 115200, 0.5,
                            auto_enter=False)
        try:
            link.hello()
            device.drop_next_type = 11     # drop the first GetBootInfo reply
            info = bootctl.read_boot_info(link)
            assert info.TargetSlot() in (Slot.A, Slot.B)

            bootctl.send_update_begin(link, {
                "version": 0x00010000, "image_size": 8,
                "image_crc32": 0, "header_crc32": 0,
                "load_address": bootctl.LOAD_ADDRESS,
                "entry_address": 0x0800C001, "build_timestamp": 0,
                "git_commit": bytes(20), "name": b"test",
            })
            assert bootctl.send_update_data(link, 100, b"abcd") == 0
            assert bootctl.send_update_data(link, 0, b"abcd") == 4
        finally:
            link.close()
    finally:
        device.stop.set()
        device.join(timeout=2)


def test_factory_hex_layout(tmp_path):
    image_a = _make_image(tmp_path, "a.img")
    image_b = _make_image(tmp_path, "b.img")
    boot = tmp_path / "boot.bin"
    boot.write_bytes(b"BOOT")
    out = tmp_path / "factory.hex"
    segments = mkimage.factory_segments(boot, image_a, image_b, 1, 2)
    mkimage.write_ihex(out, segments)

    flat = {}
    for address, data in mkimage.read_ihex(out):
        for index, byte in enumerate(data):
            flat[address + index] = byte

    def read_range(start: int, length: int) -> bytes:
        return bytes(flat[start + index] for index in range(length))

    assert read_range(0x08000000, 4) == b"BOOT"
    for base, image, meta, sequence in (
            (0x0800C000, image_a, 0x08008000, 1),
            (0x0810C000, image_b, 0x08108000, 2)):
        _header, body, crc = mkimage.read_image(image)
        assert read_range(base, len(body)) == body
        assert read_range(meta, 4) == b"IBWH"
        activated = struct.unpack_from(
            "<I", read_range(meta + 0x120 + 4, 4), 0)[0]
        assert activated == sequence


def test_lost_activate_ack_is_success_pending():
    device = FakeBoot()
    device.start()
    old_timeout = bootctl.ACTIVATE_TIMEOUT
    bootctl.ACTIVATE_TIMEOUT = 0.2
    try:
        link = bootctl.Link(device.slave_name, 115200, 0.2,
                            auto_enter=False)
        try:
            link.hello()
            device.drop_all_types.add(17)   # Activate
            assert bootctl.activate_slot(link, "B") == "pending"
        finally:
            link.close()
        assert device.active == "B"
        assert device.role != Role.BOOTLOADER
    finally:
        bootctl.ACTIVATE_TIMEOUT = old_timeout
        device.stop.set()
        device.join(timeout=2)


def test_wait_releases_boot_caught_in_listen_window(monkeypatch):
    """ENT-001: a --wait Hello inside BOOT's listen window parks BOOT in
    update mode with the new slot still pending.  wait must reset BOOT so
    the slot boots, instead of timing out with "still in BOOT"."""
    monkeypatch.setattr(bootctl, "BOOT_SETTLE_S", 0.1, raising=False)
    device = FakeBoot()
    device.active_pending = True
    device.start()
    try:
        result = bootctl.wait_for_confirmation(device.slave_name, 115200,
                                               10.0)
        assert result["state"] == "confirmed"
        assert device.resets == 1
    finally:
        device.stop.set()
        device.join(timeout=2)


def test_catch_waits_until_boot_answers():
    """catch keeps sending Hello through a reset until BOOT answers, and
    never sends EnterBootloader to the application."""
    device = FakeBoot()
    device.role = 3          # application answering as itself
    device.start()
    threading.Timer(0.4, lambda: setattr(device, "role",
                                         Role.BOOTLOADER)).start()
    try:
        link = bootctl.Link(device.slave_name, 115200, 1.0,
                            auto_enter=False)
        try:
            code = bootctl.cmd_catch(link, argparse.Namespace(
                catch_timeout=5.0))
        finally:
            link.close()
        assert code == 0
        assert device.role == Role.BOOTLOADER
    finally:
        device.stop.set()
        device.join(timeout=2)


def test_cross_repo_contract_check(tmp_path):
    repo = tmp_path / "app"
    repo.mkdir()
    defines = {
        "BOOT_BANK1_BASE": "0x08000000UL",
        "BOOT_BANK2_BASE": "0x08100000UL",
        "BOOT_BANK_SIZE": "0x00100000UL",
        "BOOT_META_OFFSET": "0x00008000UL",
        "BOOT_META_SIZE": "0x00004000UL",
        "BOOT_SLOT_OFFSET": "0x0000C000UL",
        "BOOT_SLOT_SIZE": "0x000F4000UL",
        "BOOT_META_VERIFIED_OFFSET": "0x100UL",
        "BOOT_META_ACTIVATED_OFFSET": "0x120UL",
        "BOOT_META_CONFIRMED_OFFSET": "0x140UL",
        "BOOT_META_REJECTED_OFFSET": "0x160UL",
        "BOOT_META_TRIAL_OFFSET": "0x180UL",
    }
    header = repo / "boot_contract.h"
    header.write_text("#define BOOT_APP_LOAD_ADDR 0x0800C000UL\n" +
                      "".join(f"#define {name} {value}\n"
                              for name, value in defines.items()))
    schema = repo / "hwlink.fbs"
    schema.write_text("// header_crc32 active_valid target_slot force\n")
    (repo / "memory.x").write_text("ORIGIN = 0x0800C000, LENGTH = 976K\n")
    assert check_contracts.check_repo(repo, "app") == []


def test_published_hwlink_bundle(tmp_path):
    output = tmp_path / "soccentric-hwlink"
    assert package_hwlink.package(output) == 0
    assert (output / "protocol" / "hwlink.fbs").is_file()
    assert (output / "hwlink" / "BootInfo.py").is_file()
    assert (output / "flatbuffers" / "__init__.py").is_file()
    assert (output / "include" / "boot_layout.h").is_file()
    assert (output / "mkimage.py").is_file()
    assert (output / "SHA256SUMS").is_file()
    assert "target_slot" in (output / "protocol" / "hwlink.fbs").read_text()


def test_both_loop_on_simulated_device(tmp_path):
    image_a = _make_image(tmp_path, "a.img")
    image_b = _make_image(tmp_path, "b.img")
    device = FakeBoot()
    device.start()
    try:
        link = bootctl.Link(device.slave_name, 115200, 2.0)
        args = argparse.Namespace(
            image=None, a=str(image_a), b=str(image_b),
            both=True, chunk=256, activate=False, fast=False, force=False,
            wait=False, wait_timeout=10.0, timeout=2.0, json=False,
            verbose=False)
        try:
            assert bootctl.cmd_update(link, args) == 0
        finally:
            link.close()
        assert device.active in ("A", "B")
        assert device.target in ("A", "B")
        assert device.active != device.target
    finally:
        device.stop.set()
        device.join(timeout=2)


# ------------------------------------------------------------ BSAFE-006

DEV_KEY = TOOLS.parent / "keys" / "dev-signing-key.pem"


def test_signed_image_roundtrip(tmp_path, capsys):
    image = _make_image(tmp_path, extra=["--key", str(DEV_KEY),
                                         "--security-counter", "3"])
    header, body, _crc = mkimage.read_image(image)
    assert struct.unpack_from("<H", header, 4)[0] == mkimage.HEADER_VERSION
    counter, key_id = struct.unpack_from("<II", header, 88)
    assert counter == 3
    key = mkimage.load_key(DEV_KEY)
    assert key_id == mkimage.key_id(mkimage.public_point(key))
    assert mkimage.signature_valid(header, key)

    assert mkimage.inspect(image, DEV_KEY) == 0
    assert "signature     : valid" in capsys.readouterr().out


def test_signature_detects_tampering(tmp_path):
    import zlib
    image = _make_image(tmp_path, extra=["--key", str(DEV_KEY)])
    data = bytearray(image.read_bytes())
    key = mkimage.load_key(DEV_KEY)

    # Raise the security counter and fix the CRC: only the signature
    # catches it.
    tampered = bytearray(data)
    struct.pack_into("<I", tampered, 88, 99)
    struct.pack_into("<I", tampered, 252, zlib.crc32(tampered[:252]))
    assert not mkimage.signature_valid(bytes(tampered[:256]), key)
    bad = tmp_path / "bad.img"
    bad.write_bytes(tampered)
    assert mkimage.inspect(bad, DEV_KEY) == 1

    # A changed body breaks the signed SHA-256 even with matching CRCs.
    tampered = bytearray(data)
    tampered[256 + 20] ^= 0xFF
    struct.pack_into("<I", tampered, 20, zlib.crc32(tampered[256:]))
    struct.pack_into("<I", tampered, 252, zlib.crc32(tampered[:252]))
    bad.write_bytes(tampered)
    try:
        mkimage.read_image(bad)
    except ValueError as error:
        assert "SHA-256" in str(error)
    else:
        raise AssertionError("SHA-256 mismatch not detected")

    # A different key is reported as such.
    other = tmp_path / "other.pem"
    mkimage.generate_key(other)
    assert mkimage.inspect(image, other) == 1


def test_sign_existing_unsigned_image(tmp_path):
    unsigned = _make_image(tmp_path, "unsigned.img")
    header, _body, _crc = mkimage.read_image(unsigned)
    assert struct.unpack_from("<H", header, 4)[0] == \
        mkimage.HEADER_VERSION_UNSIGNED

    signed = tmp_path / "signed.img"
    old = sys.argv
    sys.argv = ["mkimage.py", "--sign", str(unsigned), "--key", str(DEV_KEY),
                "--security-counter", "0x10", "--output", str(signed)]
    try:
        assert mkimage.main() == 0
    finally:
        sys.argv = old
    header, body, _crc = mkimage.read_image(signed)
    assert body == unsigned.read_bytes()[256:]
    assert struct.unpack_from("<I", header, 88)[0] == 16
    assert mkimage.signature_valid(header, mkimage.load_key(DEV_KEY))
    # Everything before the version 2 fields is unchanged except the
    # header version.
    original = unsigned.read_bytes()
    assert header[:4] == original[:4] and header[6:88] == original[6:88]


def test_exported_pubkey_matches_committed_source(tmp_path):
    key = mkimage.load_key(DEV_KEY)
    committed = (TOOLS.parent / "keys" / "dev_pubkey.c").read_text()
    generated = mkimage.pubkey_c_source(key, DEV_KEY.name)
    assert generated == committed


def test_update_begin_carries_signed_header(tmp_path):
    image_path = _make_image(tmp_path, extra=["--key", str(DEV_KEY),
                                              "--security-counter", "5"])
    image = bootctl.parse_image(image_path)
    assert image["header_version"] == 2
    assert image["security_counter"] == 5

    device = FakeBoot()
    device.start()
    try:
        link = bootctl.Link(device.slave_name, 115200, 0.5,
                            auto_enter=False)
        try:
            link.hello()
            bootctl.send_update_begin(link, image)
        finally:
            link.close()
    finally:
        device.stop.set()
        device.join(timeout=2)
    # BOOT rebuilds exactly the header that was signed.
    assert device.begin_header == image_path.read_bytes()[:256]
