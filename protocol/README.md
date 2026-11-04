# HWLINK protocol schema

`hwlink.fbs` is the FlatBuffers schema for the HWLINK protocol
(SRS-GUI-F767 Appendix A).  It is the single source for the C headers in
`../src/generated` and the Python package in `../tools/generated`.

The copies in the other repositories are **not** identical yet.  The GUI
adds the HWVAL tables and `OperatorPromptDone`; the validation firmware adds
the HWVAL tables and `OperatorPromptDone`; the C, Rust and Zephyr
applications omit `ImageHeader.header_crc32`.  The message set used by BOOT
and the applications is wire-compatible (same union indices, same message
layouts), and every component now reports protocol minor 1.  Merging the
copies into one canonical schema is tracked as B-1 in the workspace
`task.md`.

The C headers were generated with flatcc 0.6.1:

```sh
flatcc -a -o src/generated protocol/hwlink.fbs
```

The generated files are committed so the `stm32-dev` container does not need
a host flatcc compiler.  The runtime is vendored in `../third_party/flatcc`
under its original licence.

The Python package used by `bootctl.py` was generated with flatc 2.0.8:

```sh
flatc --python -o tools/generated protocol/hwlink.fbs
```

The Python flatbuffers runtime is vendored in `../tools/vendor`.

Protocol minor 1 appends `ImageHeader.header_crc32` (IMG-001).  The field is
optional on the wire: older hosts may omit it and BOOT recomputes the header
CRC from the remaining fields.

`Message.body` is an append-only union: add new members at the end and raise
the protocol minor version (LNK-013).  The file identifier is `HWLK`.
