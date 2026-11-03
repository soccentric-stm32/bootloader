# soccentric-stm32-bootloader

Bare-metal A/B bootloader for the NUCLEO-F767ZI (STM32F767ZI). Dual-bank flash with one application slot per bank, image updates over the ST-LINK VCP using the HWLINK protocol, CRC32 integrity checks, and trial boot with automatic rollback.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-BOOT-F767 (v0.1 draft) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Skeleton: flash-mode check (BSAFE-005), B1 stay (ENT-003), slot A vector check (IMG-003) and hand-over (HND-001/002). UART update, A/B selection and rollback still to come. |
| Build environment | [docker/README.md](docker/README.md): same `stm32-dev` image as the validation firmware (STM32CubeF7 v1.17.4, arm-none-eabi-gcc 13.2) |

## Build and flash

Everything runs in the `stm32-dev` container through `scripts/docker-dev.sh`:

```bash
scripts/build.sh            # cmake configure + ninja build (in the container)
scripts/build.sh clean      # from scratch
make flash                  # flash build/boot.elf over SWD, verify, reset
make help                   # all targets (lint, debug, serial, ...)
```

Outputs in `build/`: `boot.elf`, `boot.bin`, `boot.hex`, `boot.map`. `make flash` puts BOOT at `0x08000000`. The board must be in dual-bank mode (`nDBANK = 0`); in single-bank mode BOOT only blinks LD3 (BSAFE-005).

## Related projects

| Repository | Role |
|---|---|
| [soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware) | HWVAL hardware validation firmware |
| [soccentric-stm32-bootloader](https://github.com/soccentric-stm32/soccentric-stm32-bootloader) | A/B bootloader (UART update, CRC32, trial boot and rollback) |
| [soccentric-stm32-application](https://github.com/soccentric-stm32/soccentric-stm32-application) | FreeRTOS demo application (LEDs, button, HWLINK) |
| [soccentric-stm32-validation-gui](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui) | PySide6 GUI: runs HWVAL tests, updates and flashes firmware |

Author: Sandesh Ghimire <sandesh@soccentric.com>
