# soccentric-stm32-bootloader

Bare-metal A/B bootloader for the NUCLEO-F767ZI (STM32F767ZI). Dual-bank flash with one application slot per bank, image updates over the ST-LINK VCP using the HWLINK protocol, CRC32 integrity checks, and trial boot with automatic rollback.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-BOOT-F767 (v0.1 draft) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Specification only; no implementation yet |

## Related projects

| Repository | Role |
|---|---|
| [soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware) | HWVAL hardware validation firmware |
| [soccentric-stm32-bootloader](https://github.com/soccentric-stm32/soccentric-stm32-bootloader) | A/B bootloader (UART update, CRC32, trial boot and rollback) |
| [soccentric-stm32-application](https://github.com/soccentric-stm32/soccentric-stm32-application) | FreeRTOS demo application (LEDs, button, HWLINK) |
| [soccentric-stm32-validation-gui](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui) | PySide6 GUI: runs HWVAL tests, updates and flashes firmware |

Author: Sandesh Ghimire <sandesh@soccentric.com>
