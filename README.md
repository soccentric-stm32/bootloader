# soccentric-stm32-bootloader

Bare-metal A/B bootloader for the NUCLEO-F767ZI (STM32F767ZI). Dual-bank flash
with one application slot per bank, image updates over the ST-LINK VCP using
the HWLINK protocol, CRC32 integrity checks, and trial boot with automatic
rollback.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-BOOT-F767 (v0.1) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Feature complete for v0.1: clock tree, USART3 + HWLINK server, image/header CRC32, metadata and A/B selection, bank swap, trial boot + IWDG + rollback, `BootInfo` and the host tools |
| Build environment | [docker/README.md](docker/README.md): same `stm32-dev` image as the validation firmware (STM32CubeF7 v1.17.4, arm-none-eabi-gcc 13.2) |

## Flash layout (dual bank, `nDBANK = 0`)

| Region | Bank 1 | Bank 2 | Size |
|---|---|---|---|
| BOOT | `0x0800 0000`–`0x0800 7FFF` | reserved `0x0810 0000`–`0x0810 7FFF` | 32 KB |
| Metadata | `0x0800 8000`–`0x0800 BFFF` | `0x0810 8000`–`0x0810 BFFF` | 16 KB |
| Application slot A / B | `0x0800 C000`–`0x080F FFFF` | `0x0810 C000`–`0x081F FFFF` | 976 KB |

Slot A is in bank 1, slot B in bank 2.  The same application binary runs from
either slot: BOOT maps the active bank to `0x0800 0000` with
`SYSCFG_MEMRMP.SWP_FB` and jumps to `0x0800 C000`.  The swap and jump routine
runs from DTCM (`src/app/swap_jump.S`) because the swap remaps the bank BOOT
itself executes from.

The metadata sector is append-only.  Each record is 32 bytes: magic, one
value word, zeros and a CRC32 of the first 28 bytes.  The header (256 bytes)
is followed by `VERIFIED`, `ACTIVATED`, `CONFIRMED` and `REJECTED` records;
see SRS Appendix B.  A torn or erased record fails its CRC and is treated as
absent.

BOOT itself uses 60 KB of DTCM (code, stack, message buffers, flatcc arena)
and a 4 KB USART3 receive ring in SRAM1.  The ring must not be in DTCM: DMA
cannot access the tightly coupled memory on the STM32F7.

## Clock tree

BOOT runs at 216 MHz from the 8 MHz ST-LINK MCO (HSE bypass, PLLN = 432,
PLLP = 2, over-drive, 7 flash wait states).  USART3 is on APB1 at 54 MHz,
which keeps both 115200 and 921600 baud well inside the 1.5 % error budget.

## Repository layout

```
src/app/        Boot flow: main, slot selection, update, hand-over, swap_jump.S
src/bsp/        STM32F7 board support: clock, flash, uart3, iwdg, HardFault
src/common/     Hardware-independent: boot_layout.h, crc32, image, metadata
src/protocol/   HWLINK codec/server, flatcc port, generated FlatBuffers headers
board/          HAL configuration and board pin map
ld/             Linker script (bank 1 sectors 0-1, 32 KB)
protocol/       HWLINK FlatBuffers schema (hwlink.fbs)
tools/          Host tools: bootctl.py, mkimage.py, provisioning
scripts/        Build, Docker and serial helpers
third_party/    Vendored flatcc runtime
docker/         stm32-dev build environment
```

`src/README.md` describes the layering rules and how to add a module.

## Build and flash

Everything runs in the `stm32-dev` container through `scripts/docker-dev.sh`:

```bash
scripts/build.sh            # cmake configure + ninja build (in the container)
scripts/build.sh clean      # from scratch
make flash                  # flash build/boot.elf over SWD, verify, reset
make help                   # all targets (lint, debug, serial, ...)
```

Outputs in `build/`: `boot.elf`, `boot.bin`, `boot.hex`, `boot.map`.  `make
flash` puts BOOT at `0x08000000`.  The build fails if BOOT grows past
`0x0800 7FFF`; the current image is about 28 KB of flash.

## Product provisioning

BOOT needs dual-bank mode and dual boot disabled.  The GUI does the same over
SWD in production; for CI and manual bring-up:

```bash
tools/provision.sh                     # option bytes + mass erase + BOOT
tools/provision.sh --no-options ...    # option bytes already set
```

`tools/provision.sh` runs OpenOCD in the `stm32-dev` container.  Changing
`nDBANK` forces a mass erase, so it erases the whole flash before programming
BOOT.  If the OpenOCD version on the bench cannot write the F767 option bytes
(SRS OPEN-B05), set `nDBANK = 0` and `nDBOOT = 1` with STM32CubeProgrammer and
rerun with `--no-options`.

## Update workflow

1. Build the application, linked at `0x0800 C000` (see the application
   repositories), and wrap it into an image:

   ```bash
   tools/mkimage.py --input build/app.bin --output build/app.img \
       --version 0.1.0 --commit <sha> --timestamp <unix> \
       --name stm32-application
   tools/mkimage.py --inspect build/app.img
   ```

2. Flash BOOT over SWD, then update over the ST-LINK VCP:

   ```bash
   pip install pyserial        # only host dependency; flatbuffers is vendored
   tools/bootctl.py info
   tools/bootctl.py update build/app.img --activate
   tools/bootctl.py enter          # running application -> BOOT
   tools/bootctl.py reset
   ```

   `bootctl.py` implements `info`, `update`, `activate`, `enter`, `reset`
   and `set-baud` over HWLINK.  Add `--fast` to `update` to switch to
   921600 inside the same session (BOOT always starts at 115200).

3. On reset BOOT listens for 500 ms for a HWLINK `Hello` ([ENT-001]), then
   boots the valid slot with the highest activation sequence number.  B1
   held at reset, a `EnterBootloader` request from the application, or no
   valid slot keeps it in update mode.

An update writes only the inactive slot.  Slot sectors are erased lazily when
the first chunk that falls into them arrives (128 KB sector erase can take up
to about 2 s).  `UpdateEnd` verifies the vector table and the CRC32 of the
slot contents read back from flash before writing `VERIFIED`.  `Activate`
writes `ACTIVATED` with the next sequence number, resets, and the new image
boots on trial.

### Trial boot and rollback

A pending image is counted in `RTC_BKP0R` before every hand-over.  After three
failed starts the next boot writes `REJECTED` and selects the other slot.
BOOT starts the IWDG with a 4 s timeout, so a hung application resets and
counts as a failed trial.  The rollback is reported in `GetBootInfo` as
`ROLLED_BACK` with the rejected slot until the next successful `Activate`.
The application confirms itself by programming the `CONFIRMED` record in its
own metadata sector (now visible at `0x0800 8000` because of the bank swap).

## Recovery

Recovery is always over SWD: `make erase` erases the whole flash,
`make flash` writes BOOT again, and the application images can be fetched
from the GUI or rebuilt.  BOOT never writes option bytes, OTP, read/write
protection or its own sectors.

## Security note

Integrity is protected by CRC32 only.  BOOT does **not** authenticate images:
anyone with access to the UART can load an image (SRS BSAFE-006).  Signing is
out of scope for v0.1.

## Related projects

| Repository | Role |
|---|---|
| [soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware) | HWVAL hardware validation firmware |
| [soccentric-stm32-bootloader](https://github.com/soccentric-stm32/soccentric-stm32-bootloader) | A/B bootloader (UART update, CRC32, trial boot and rollback) |
| [soccentric-stm32-application](https://github.com/soccentric-stm32/soccentric-stm32-application) | FreeRTOS demo application in C (LEDs, button, HWLINK) |
| [soccentric-stm32-rust-application](https://github.com/soccentric-stm32/soccentric-stm32-rust-application) | The same demo in Rust (Embassy) |
| [soccentric-stm32-zephyr-application](https://github.com/soccentric-stm32/soccentric-stm32-zephyr-application) | The same demo on Zephyr RTOS |
| [soccentric-stm32-validation-gui](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui) | PySide6 GUI: runs HWVAL tests, updates and flashes firmware |
| [soccentric-stm32-factory-provisioning](https://github.com/soccentric-stm32/soccentric-stm32-factory-provisioning) | HWVAL Factory: end-of-line test and provisioning station |

## Third-party code

`third_party/flatcc` is the Apache-2.0 flatcc 0.6.1 runtime, used as released.
`tools/vendor/flatbuffers` is the Apache-2.0 Python FlatBuffers runtime used
by `bootctl.py`.  The C headers in `src/protocol/generated` and the Python package in
`tools/generated` are generated from `protocol/hwlink.fbs`.  STM32CubeF7 is
fetched by the Docker image and keeps its own licence.

## License

[MIT](LICENSE) © 2026 Sandesh Ghimire. Use it as a starting point for your own
products. Third-party code (STM32CubeF7, FreeRTOS, LwIP, Embassy, Zephyr and
others) is fetched at build time and keeps its own licence.

Author: Sandesh Ghimire <sandesh@soccentric.com>
