# bootloader

A bare-metal A/B bootloader for the NUCLEO-F767ZI (STM32F767ZI). It uses the dual-bank flash of the STM32F7 to hold two application slots, receives new images over the ST-LINK virtual COM port using the HWLINK protocol, checks every image with CRC32 and boots a new image on trial. If the application does not confirm itself within three starts, the bootloader rolls back to the last confirmed slot automatically. Host tools for building images, updating devices and factory provisioning are included, together with host and protocol tests. It is the foundation every application in this project family runs on.

## Debug terminal

The debug log from a reset on a NUCLEO-F767ZI (USART6, 115200 8N1), recorded from the board. [Watch it replay line by line](https://soccentric-stm32.github.io/#bootloader) on the project page.

![Debug terminal output from a reset on a NUCLEO-F767ZI](docs/debug-terminal.svg)

<details>
<summary>Plain text</summary>

```console
$ openocd -f board/st_nucleo_f7.cfg -c "init; reset run; shutdown"
[      0] BOOT I start v0.1.0 d389f50d03e7, reset 0x04000000
[      5] BOOT I init: clock 216 MHz, PLL locked
[      9] BOOT I init: HWLINK on USART3, 115200 8N1
[     14] BOOT I scan: searching for an application
[    335] BOOT I init: flash dual bank, writes enabled
[    339] BOOT I slot A: ok, confirmed
[    343] BOOT I slot B: ok, confirmed
[    346] BOOT I sel: slot A confirmed, count 0
[    351] BOOT I listen: 500 ms for a host
[    854] BOOT I boot slot A
[      1] C-FRT I start v0.1.0 d501c03a148d, slot A confirmed, reset reset pin
[      1] C-FRT I task 3 Health: watches task check-ins, kicks the watchdog, confirms the slot
[      1] C-FRT I task 4 Comms: answers the factory station: reads and writes the factory data window
[      1] C-FRT I task 2 Button: debounces B1, short press cycles the LED pattern, long press resets it
[      1] C-FRT I task 1 LED: drives LD1-LD3 patterns, LD3 shows health and fault state
```

</details>

## Features

- Dual-bank A/B layout: 32 KB bootloader, 16 KB metadata and a 976 KB slot per bank
- Same application binary runs from either slot through the bank-swap remap
- Image updates over UART with the HWLINK protocol (COBS, CRC-32, FlatBuffers)
- 115200 baud by default, 921600 baud on request within a session
- CRC32 check of the image header and of the slot contents read back from flash
- Signed images: ECDSA P-256 over a header that carries the image SHA-256, checked before an update erases anything and on every boot
- Anti-rollback security counter held in the signed headers, with no OTP or option-byte writes
- Trial boot with an IWDG watchdog and automatic rollback after three failed starts
- Append-only, power-loss-safe metadata records (VERIFIED, ACTIVATED, CONFIRMED, REJECTED, TRIAL)
- Lazy sector erase so only the sectors an update touches are erased
- 500 ms HWLINK listen window at reset; B1 button forces update mode
- `GetBootInfo` reports both slots, the active slot and rollback state
- 216 MHz clock tree from the ST-LINK MCO
- About 28 KB of flash; the build fails if it outgrows 32 KB
- Never writes option bytes, OTP, protection or its own sectors
- `bootctl.py` host tool: info, update, activate, enter, wait, reset, set-baud, JSON output
- The BOOT Flasher GUI (build and flash BOOT, sign and flash the six firmware projects) is in [bootloader-gui](https://github.com/soccentric-stm32/bootloader-gui)
- `mkimage.py`: image wrapping, signing, key generation, inspection, metadata sectors and combined factory HEX
- `provision.sh`: option bytes, mass erase and bootloader programming over SWD
- Host C tests against a simulated 2 MB flash
- Python tests including a simulated bootloader on a pseudo-terminal
- Layout, schema and protocol consistency checks
- Optional hardware-in-the-loop CI stage with a rollback test
- Reproducible Docker build environment

## Build and run

```bash
scripts/build.sh                                   # build inside the stm32-dev container
make flash                                         # flash build/boot.elf over SWD
make test                                          # host, Python and contract tests
tools/mkimage.py --sign app.img --key keys/dev-signing-key.pem --output app-signed.img
tools/bootctl.py update app-signed.img --activate --wait  # update an application over UART
```

BOOT only accepts signed images.  The default build trusts the published
development key in [keys/](keys/README.md), which is fine on the bench but must
never ship: generate a product key and build with
`-DBOOT_SIGNING_KEY_SOURCE=<path to its pubkey .c>`. The full guide, including how to sign
application images, is docs/IMAGE_SIGNING.md.

## Related projects

- [app-c-freertos](https://github.com/soccentric-stm32/app-c-freertos) — Demo application in C on FreeRTOS
- [app-c-zephyr](https://github.com/soccentric-stm32/app-c-zephyr) — Demo application in C on Zephyr RTOS
- [app-rust-freertos](https://github.com/soccentric-stm32/app-rust-freertos) — Demo application in Rust on Embassy
- [app-rust-zephyr](https://github.com/soccentric-stm32/app-rust-zephyr) — Demo application in Rust on Zephyr RTOS
- [validation-firmware](https://github.com/soccentric-stm32/validation-firmware) — HWVAL hardware validation firmware
- [bootloader-gui](https://github.com/soccentric-stm32/bootloader-gui) — BOOT Flasher: desktop GUI to flash BOOT and the firmware projects
- [validation-gui](https://github.com/soccentric-stm32/validation-gui) — HWVAL Studio: desktop GUI for tests, updates and flashing
- [factory-station](https://github.com/soccentric-stm32/factory-station) — HWVAL Factory: end-of-line test and provisioning station
- [soccentric-stm32.github.io](https://github.com/soccentric-stm32/soccentric-stm32.github.io) — Project website

## License

Released under the [MIT License](LICENSE) © 2026 Sandesh Ghimire. You are free
to use, modify and ship it in your own products.

Contributions are welcome. Fork the repository, make your change on a branch and
open a pull request. Bug reports, ideas and improvements are all appreciated.
