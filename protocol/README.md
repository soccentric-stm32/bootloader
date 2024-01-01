# HWLINK protocol schema

`hwlink.fbs` is the FlatBuffers schema for the HWLINK protocol
(SRS-GUI-F767 Appendix A).  It is the single source for the C headers in
`../src/protocol/generated` and the Python package in `../tools/generated`.

Protocol minor 3 (BSAFE-006) appends `ImageHeader.extension` (header bytes
88..251 verbatim: security counter, key id, image SHA-256 and signature),
`BootSlotInfo.security_counter`, `BootInfo.signed_images`, `BootInfo.key_id`,
`BootInfo.security_floor` and the errors `AUTH_FAILED` and `ROLLBACK`.  BOOT
rebuilds the 256-byte header from the fields, so the extension must arrive
unchanged for the signature to verify.

Protocol minor 2 appends `BootInfo.active_valid`, `BootInfo.target_slot`,
`BootInfo.boot_version`, `BootInfo.boot_build` and `UpdateBegin.force`.
Hosts must use `target_slot` instead of guessing which slot BOOT will write,
and `force` is a bench-only override for an update while the active slot is
still on trial.  Older minors remain wire-compatible because all new fields
are appended.

The copies in the other repositories must follow the same append-only rule.
`tools/check_contracts.py --app-repo <repo>` checks the layout constants,
`header_crc32`, `active_valid`, `target_slot` and `force` in each copy and
can run in CI.  `make package-hwlink` publishes the canonical schema,
generated Python package, `mkimage.py`, `boot_layout.h` and the vendored
FlatBuffers runtime as one `build/soccentric-hwlink` bundle (P3-8).

The C headers were generated with flatcc 0.6.1:

```sh
flatcc -a -o src/protocol/generated protocol/hwlink.fbs
```

The generated files are committed so the `stm32-dev` container does not need
a host flatcc compiler.  The runtime is vendored in `../third_party/flatcc`
under its original licence.

The Python package used by `bootctl.py` was generated with flatc 2.0.8:

```sh
flatc --python -o tools/generated protocol/hwlink.fbs
```

The Python flatbuffers runtime is vendored in `../tools/vendor`.

Protocol minor 1 appended `ImageHeader.header_crc32` (IMG-001).  The field
is optional on the wire: older hosts may omit it and BOOT recomputes the
header CRC from the remaining fields.  Protocol minor 2 appends the BootInfo
and UpdateBegin fields listed above, and minor 3 the signing fields;
`bootctl.py` reports minor 3.

`Message.body` is an append-only union: add new members at the end and raise
the protocol minor version (LNK-013).  The file identifier is `HWLK`.
