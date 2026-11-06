# Image signing and anti-rollback

Author: Sandesh Ghimire <sandesh@soccentric.com>
Date: 2026-11-05
SRS: BSAFE-006, BSAFE-007, BSAFE-008

BOOT only starts application images signed with the key built into it, and
refuses images older than the newest confirmed one. This guide covers how it
works, how to set up a board, and how to sign images: by hand, inside an
application build and in CI.

## Contents

1. [How it works](#1-how-it-works)
2. [Prerequisites](#2-prerequisites)
3. [Prepare the board](#3-prepare-the-board)
4. [Build and flash BOOT](#4-build-and-flash-boot)
5. [Sign an image by hand](#5-sign-an-image-by-hand)
6. [Update a device](#6-update-a-device)
7. [Add signing to an application](#7-add-signing-to-an-application)
8. [Choosing the security counter](#8-choosing-the-security-counter)
9. [Product keys](#9-product-keys)
10. [Factory provisioning](#10-factory-provisioning)
11. [CI](#11-ci)
12. [Troubleshooting](#12-troubleshooting)
13. [Reference](#13-reference)

---

## 1. How it works

An application image is a 256-byte header followed by the binary. Header
version 2 adds four fields in what used to be the reserved area:

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 88 | 4 | `security_counter` | Anti-rollback counter |
| 92 | 4 | `key_id` | First 4 bytes of SHA-256 of the public key, little-endian |
| 96 | 32 | `image_sha256` | SHA-256 of the binary |
| 128 | 64 | `signature` | ECDSA P-256 `r ‖ s`, big-endian, over SHA-256 of header bytes 0–127 |
| 192 | 60 | `reserved` | Zero |
| 252 | 4 | `header_crc32` | CRC32 of bytes 0–251 |

The signature covers every field before it, including `image_sha256`. One
signature therefore authenticates both the header and the binary: changing
either breaks it.

**Where BOOT checks**

| When | What is checked | Error on failure |
| --- | --- | --- |
| `UpdateBegin`, before anything is erased | header version 2, key id, signature, counter ≥ floor | `AUTH_FAILED`, `ROLLBACK` |
| `UpdateEnd` | SHA-256 of the bytes read back from flash | `AUTH_FAILED` |
| `Activate` | all of the above again | `BAD_STATE`, `ROLLBACK` |
| Every boot | signature of both slot headers, SHA-256 of the slot about to start, floor | slot not bootable |

An unsigned or forged image is refused before BOOT erases anything, so it
cannot even destroy the inactive slot. The bench-only `--force` option does
not bypass any of these checks.

**Anti-rollback.** The *floor* is the highest `security_counter` among slots
whose header is authentic, which the application has CONFIRMED, and which
are not REJECTED. BOOT refuses to install, activate or boot anything below
it. The floor lives in the signed headers, so BOOT never writes OTP or
option bytes. It rises when the application confirms a newer image.

```
A: counter 1, confirmed      floor = 1
B: counter 2, pending        boots on trial; A is still the rollback image
B confirms                   floor = 2  ->  A is "superseded", not bootable
```

---

## 2. Prerequisites

- Docker with the `stm32-dev` image (`make image`), used for building and
  for OpenOCD
- Python 3.10+ with `pyserial` and `cryptography`:

  ```bash
  pip install -r tools/requirements.txt
  ```

- A NUCLEO-F767ZI connected through the ST-LINK USB port

---

## 3. Prepare the board

BOOT needs dual-bank flash and software watchdogs. Check the option bytes:

```bash
scripts/docker-dev.sh run openocd -f board/st_nucleo_f7.cfg \
    -c "adapter speed 1000" -c "init" -c "reset halt" \
    -c "stm32f2x options_read 0" -c "shutdown"
```

The expected value is `user_options 0x1F4`:

| Bit | Option | Required | Symptom if wrong |
| --- | --- | --- | --- |
| 9 | `nDBANK` | 0 (dual bank) | openocd says "Single Bank"; BOOT reports `WRONG_FLASH_MODE` |
| 8 | `nDBOOT` | 1 | |
| 4 | `WWDG_SW` | 1 (software) | the board resets every few tens of ms and BOOT never answers |
| 5 | `IWDG_SW` | 1 (software) | |

If any value is wrong, provision the board. **Provisioning erases the whole
2 MB flash**, including the factory identity sector:

```bash
make build
tools/provision.sh build/boot.elf
```

`provision.sh` sets `nDBANK=0`, `nDBOOT=1` and software watchdogs, keeps the
other option bytes, mass-erases the flash and writes BOOT.

---

## 4. Build and flash BOOT

```bash
make build      # warns that BOOT trusts the development key
make flash
python3 tools/bootctl.py --port /dev/ttyACM0 info
```

`info` should show:

```
flash mode   : DUAL_BANK
signing      : required, key id 0xfaa7171b, security floor 0
```

`0xfaa7171b` is the development key. BOOT is about 31 KB of its 32 KB.

**Build options**

| CMake option | Default | Purpose |
| --- | --- | --- |
| `BOOT_SIGNED_IMAGES` | `ON` | `OFF` builds a bench BOOT that also accepts unsigned version 1 images |
| `BOOT_SIGNING_KEY_SOURCE` | `keys/dev_pubkey.c` | C file with the public key BOOT trusts ([§9](#9-product-keys)) |

The scripts run OpenOCD at 1 MHz. At the board configuration's 2 MHz the
flash write timed out on every attempt. Override it with `OPENOCD_SPEED=<kHz>`.

---

## 5. Sign an image by hand

**Sign an image an application build already produced:**

```bash
tools/mkimage.py --sign ../app-c-freertos/build/demo.img \
    --key keys/dev-signing-key.pem --security-counter 2 \
    --output build/demo-signed.img
```

`--sign` keeps every existing header field, sets the header version to 2,
fills in the signature fields and recomputes the header CRC. Without
`--security-counter` it keeps the counter already in the header (0 for an
unsigned image).

**Or wrap and sign a `.bin` in one step:**

```bash
tools/mkimage.py --input app.bin --output app.img \
    --version 1.4.0 --commit "$(git rev-parse HEAD)" \
    --timestamp "$(date +%s)" --name stm32-application \
    --key keys/dev-signing-key.pem --security-counter 2
```

**Check an image:**

```bash
tools/mkimage.py --inspect build/demo-signed.img --key keys/dev-signing-key.pem
```

```
security ctr  : 2
key id        : 0xfaa7171b
image SHA-256 : 4f63...
signature     : valid
OK
```

`--inspect` also accepts a public-key PEM, so a release can be verified
without the private key.

---

## 6. Update a device

```bash
python3 tools/bootctl.py --port /dev/ttyACM0 update build/demo-signed.img --activate --wait
```

BOOT writes the inactive slot, activates it and boots it on trial. The
application confirms itself after its health check (about 5 s in
app-c-freertos), and from then on it is the rollback floor.

To fill both slots:

```bash
python3 tools/bootctl.py --port /dev/ttyACM0 update --both --a a.img --b b.img
```

`bootctl info` shows each slot's security counter and the current floor. If
the board is running the application, `bootctl` switches it to BOOT first.

**From the flash GUI.** `make gui`, then pick one of the five firmware
projects in the sidebar (app-c-freertos, app-c-zephyr, app-rust-freertos,
app-rust-zephyr, validation-firmware) and press **Build, Sign & Flash**:

1. The GUI clones the repository if it is missing, runs its build, signs
   the image from [§7](#7-add-signing-to-an-application) into
   `<image>-signed.img` with `mkimage.py --sign`, checks it with
   `--inspect`, and uploads it with `bootctl update --activate --wait`.
2. The signing key defaults to `BOOT_SIGNING_KEY`, or the development key.
   The Board panel shows BOOT's key id and floor, and the page warns
   before an upload BOOT would refuse, or one that raises the floor.
3. The counter defaults to *Match board*: the image is signed with the
   board's floor, so it installs without raising it
   ([§8](#8-choosing-the-security-counter)).

Signing needs `cryptography` in the GUI's Python (`make gui-deps`).

---

## 7. Add signing to an application

Applications need no firmware change for signing: BOOT checks the image, not
the running code. Each application must:

1. **Produce a signed image.**
2. **Write CONFIRMED after its health check**, as all four applications
   already do. Its `GetStatus` must then report trial `CONFIRMED`, or
   `bootctl --wait` times out even though the update worked. This was fixed
   in app-c-freertos (2ead489); check the other applications.

Application builds run inside a container that does not have the Python
`cryptography` package, so the recommended approach signs on the host after
the build.

### Option A (recommended): a host-side `sign` target

Add this to the application's `Makefile`:

```make
BOOT_DIR         ?= ../bootloader
SIGNING_KEY      ?= $(BOOT_DIR)/keys/dev-signing-key.pem
SECURITY_COUNTER ?= 0
IMAGE            ?= build/demo.img        # see the table below for each app
SIGNED_IMAGE     ?= $(IMAGE:.img=-signed.img)

.PHONY: sign
sign: build
	python3 $(BOOT_DIR)/tools/mkimage.py --sign $(IMAGE) \
		--key $(SIGNING_KEY) --security-counter $(SECURITY_COUNTER) \
		--output $(SIGNED_IMAGE)
	python3 $(BOOT_DIR)/tools/mkimage.py --inspect $(SIGNED_IMAGE) \
		--key $(SIGNING_KEY)
```

Then:

```bash
make sign                                   # development key, counter 0
make sign SECURITY_COUNTER=3 SIGNING_KEY=/secure/product-signing-key.pem
```

This keeps the application's own `scripts/mkimage.py` unchanged, and the
private key never enters the build container.

### Option B: sign inside the CMake build

1. Replace the application's `scripts/mkimage.py` with
   `bootloader/tools/mkimage.py`. `make package-hwlink` in the bootloader
   publishes the canonical copy.
2. Add `python3-cryptography` to the application's Docker image.
3. Extend the `mkimage.py` command in `CMakeLists.txt`:

   ```cmake
   set(APP_SIGNING_KEY "" CACHE FILEPATH "P-256 PEM key; empty = unsigned image")
   set(APP_SECURITY_COUNTER 0 CACHE STRING "Anti-rollback counter")
   if(APP_SIGNING_KEY)
       set(APP_SIGN_ARGS --key "${APP_SIGNING_KEY}"
                         --security-counter "${APP_SECURITY_COUNTER}")
   endif()

   COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mkimage.py
           --input ${CMAKE_BINARY_DIR}/demo.bin
           --output ${CMAKE_BINARY_DIR}/demo.img
           ...existing arguments...
           ${APP_SIGN_ARGS}
   ```

The key path must be visible inside the container (`/work/...`).

### Images per application

| Application | Image produced by its build |
| --- | --- |
| app-c-freertos | `build/demo.img` |
| app-c-zephyr | `build/zephyr-app.img` |
| app-rust-freertos | `build/rust-app.img`, made by `scripts/build.sh`; its `make flash` uploads this through BOOT, so point it at the signed image |
| app-rust-zephyr | `build/rust-zephyr-app.img` |

Updating each application's copy of `hwlink.fbs` to protocol minor 3 is
optional, because the applications do not use the new fields.
`tools/check_contracts.py --app-repo <repo>` still passes either way.

---

## 8. Choosing the security counter

- The counter is separate from the version string. Use it only to block
  downgrades.
- **Keep it the same** for ordinary releases. Devices can then still roll
  back to the previous release if a trial fails.
- **Raise it** only for a release that fixes a security problem. Once a
  device confirms that release, every image with a lower counter is
  permanently refused on that device, and the old image in the other slot
  stops being bootable.
- Never reuse a counter for a vulnerable build after raising it.
- The bench board used for the hardware test has floor 2. Sign bench images
  with `--security-counter 2` or higher.

---

## 9. Product keys

The development key `keys/dev-signing-key.pem` is published in this
repository. **Anyone can sign images with it.** Never ship a BOOT that
trusts it; the build prints a CMake warning whenever it does.

1. **Generate the key**, on an offline machine or in an HSM:

   ```bash
   tools/mkimage.py --genkey /secure/product-signing-key.pem
   ```

2. **Export the public key as C.** The output contains only the public key
   and is safe to commit to a product repository:

   ```bash
   tools/mkimage.py --export-pubkey /secure/product-signing-key.pem \
       --output /secure/product_pubkey.c
   ```

3. **Build BOOT with it:**

   ```bash
   scripts/docker-dev.sh run cmake -S /work -B /work/build -G Ninja \
       -DBOOT_SIGNING_KEY_SOURCE=/work/keys/product_pubkey.c
   scripts/docker-dev.sh run cmake --build /work/build
   ```

4. **Check it:** `bootctl info` shows the new key id. Images signed with the
   development key are now refused with "signed with a different key".

**Rules**

- Never commit a private key. `.gitignore` ignores every `*.pem` except the
  development key.
- Keep the private key in an HSM or as a CI secret, and sign only in the
  release job.
- To rotate a key, build and flash a BOOT with the new public key over SWD.
  BOOT is not updated over UART (OPEN-B04).
- Signing protects the UART update path and the boot-time check. Without
  read-out protection, anyone with SWD access can replace BOOT itself.

---

## 10. Factory provisioning

Signed images work with the combined factory HEX:

```bash
tools/mkimage.py --factory-hex build/factory.hex --boot build/boot.hex \
    --slot-a app-signed.img --slot-b app-signed.img
tools/provision.sh build/factory.hex
```

Both slot images must be signed with the key BOOT trusts, or the board boots
straight into update mode. The metadata sectors mark both slots CONFIRMED,
so the floor starts at the higher of the two counters.

---

## 11. CI

- The Jenkins Python tests install `tools/requirements.txt`, which now
  includes `cryptography`.
- The hardware-in-the-loop stage signs its deliberately broken rollback
  image with the development key. The `HIL_IMAGE_A` and `HIL_IMAGE_B`
  parameters must be signed images, and the HIL agent's `python3` needs
  `cryptography`.
- Release jobs should sign with the product key from the secret store and
  run `mkimage.py --inspect --key <public key>` on the artefact before
  publishing it.

Tests:

```bash
make test       # host C tests, Python tests, contract check
```

The host tests sign headers in C with a dedicated test key
(`tests/host/test_vectors.c`). That file also holds a header signed by
`mkimage.py`, which proves the Python signer and the firmware verifier
agree. Regenerate it with `python3 tests/host/gen_vectors.py`.

---

## 12. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `AUTH_FAILED: image is not signed` | version 1 image | `mkimage.py --sign` |
| `AUTH_FAILED: image is signed with a different key` | key does not match BOOT's | sign with the key `bootctl info` names, or rebuild BOOT |
| `AUTH_FAILED: image signature is invalid` | header changed after signing | sign again; never edit a signed header |
| `AUTH_FAILED: image SHA-256 mismatch` (UpdateEnd) | binary differs from what was signed, or a flash fault | rebuild and sign the image again |
| `ROLLBACK: security counter below the installed image` | counter below the floor | sign with a counter ≥ the floor in `bootctl info` |
| slot detail `superseded: security counter below floor` | a newer image was confirmed | expected; the slot is replaced on the next update |
| slot detail `image is not signed` / `bad signature` | slot written by an old tool or BOOT build | update the slot with a signed image |
| `bootctl --wait` times out, but `info` shows `confirmed` | application reports PENDING after confirming | fix the application's `GetStatus` ([§7](#7-add-signing-to-an-application)) |
| `bootctl: timeout waiting for reply` and the board keeps resetting | hardware WWDG option byte | [§3](#3-prepare-the-board) |
| openocd "Single Bank" | `nDBANK=1` | `tools/provision.sh` |
| openocd "timed out while waiting for target halted" during a write | SWD clock too fast | `OPENOCD_SPEED=480` |
| `signing needs the Python 'cryptography' package` | missing dependency | `pip install -r tools/requirements.txt` |

---

## 13. Reference

**Measured on a NUCLEO-F767ZI at 216 MHz (2026-11-05)**

| Item | Value |
| --- | --- |
| BOOT size | 31.1 KB of 32 KB (LTO on) |
| ECDSA P-256 verify | about 160 ms each |
| SHA-256 | about 0.87 ms per KB |
| Extra boot time, 40 KB image | about 0.35 s (two header checks plus one hash) |
| Extra boot time, full 976 KB slot | about 1.2 s |

These numbers come from HWLINK request timings, not from timing the boot
path directly. BOOT runs with the instruction and data caches off; enabling
them would shorten both checks.

**Protocol minor 3 additions** (`protocol/hwlink.fbs`)

- `ImageHeader.extension`: header bytes 88–251, sent unchanged
- `BootSlotInfo.security_counter`
- `BootInfo.signed_images`, `BootInfo.key_id`, `BootInfo.security_floor`
- `Error.AUTH_FAILED` (14), `Error.ROLLBACK` (15)

**`mkimage.py` options for signing**

| Option | Purpose |
| --- | --- |
| `--key PEM` | sign while wrapping (private key), or check with `--inspect` (private or public key) |
| `--security-counter N` | anti-rollback counter (decimal or `0x`) |
| `--sign IMG --output OUT` | sign an existing image |
| `--inspect IMG [--key PEM]` | print the header and check the signature |
| `--genkey PEM` | generate a P-256 private key (never overwrites) |
| `--export-pubkey PEM --output FILE.c` | C definition of the public key for BOOT |

**Source files**

| File | Role |
| --- | --- |
| `src/common/image.c` | header, signature and digest checks |
| `src/common/sha256.c` | SHA-256 |
| `src/common/signing_key.h` | public key declaration |
| `src/app/selection.c` | floor and boot-time checks |
| `src/app/update.c` | update-path checks |
| `third_party/micro-ecc/` | ECDSA verify (BSD-2-Clause) |
| `keys/` | development key and its C public key |
