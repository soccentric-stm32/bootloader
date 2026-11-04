# HWLINK protocol schema

`hwlink.fbs` is the FlatBuffers schema for the HWLINK protocol
(SRS-GUI-F767 Appendix A).  It is the single source for the C headers in
`../src/generated` and the Python package in `../tools/generated`, and is
shared unchanged with HWVAL, the GUI and the applications.

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
