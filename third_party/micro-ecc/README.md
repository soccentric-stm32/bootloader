# micro-ecc

This directory contains micro-ecc from <https://github.com/kmackay/micro-ecc>
at commit `541b3a78026420a3e369c4c9281c396b5e531113` (2024-11-13), used as
released under the BSD 2-Clause License (see `LICENSE.txt`).

BOOT uses only `uECC_verify()` on secp256r1 (BSAFE-006).  The build defines
which curves and options are compiled in (see `CMakeLists.txt`); signing and
key generation are never called and are removed by `--gc-sections`.  The
host unit tests also build this copy, so the firmware and the tests run the
same code.

Only the files the build needs are kept; the examples, tests and build
scripts from upstream are not included.
