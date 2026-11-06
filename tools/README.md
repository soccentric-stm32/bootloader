# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header (`--key` signs it, BSAFE-006); `--sign` signs an existing image; `--inspect [--key]` checks an image file and its signature; `--genkey` and `--export-pubkey` manage signing keys; `--metadata` builds a 16 KB metadata sector and `--factory-hex` combines BOOT + A/B + metadata (P1-4). |
| `bootctl.py` | HWLINK command-line updater: `info`, `update` (`--both`, `--a/--b`, `--json`), `activate`, `enter`, `wait`, `reset`, `set-baud`. |
| `check_contracts.py` | Cross-check layout constants, generated schema and an application repo's `boot_contract.h`/`hwlink.fbs`. |
| `package_hwlink.py` | Publish the canonical `hwlink.fbs`, generated Python package, `mkimage.py`, `boot_layout.h` and vendored runtime bundle (P3-8). |
| `gui/device.py` | Normalises `bootctl --json` BootInfo and shares ST-LINK port discovery with `bootctl.py`. |
| `gui/runner.py` | Sequential QProcess queue used by the GUI production workflow. |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |
| `flash_gui.py` | PySide6 GUI: flash BOOT over SWD, or update an application image over HWLINK with a slot suggestion. |

`bootctl.py` needs only pyserial:

```sh
pip install -r tools/requirements.txt
tools/bootctl.py --port /dev/ttyACM0 info
tools/bootctl.py --port /dev/ttyACM0 info --json   # machine-readable
tools/bootctl.py --port /dev/ttyACM0 -v update build/app.img --activate --wait
tools/bootctl.py --port /dev/ttyACM0 update --both --a a.img --b b.img --json
```

If a running application answers `Hello`, `bootctl` sends
`EnterBootloader`, reopens the port and retries BOOT for a few seconds.
`--no-enter` keeps the old behaviour.  `--both` uses BOOT's reported
`target_slot`, waits for the application's `GetStatus` to report
`CONFIRMED`, then enters BOOT again for the other slot.

`-v/--verbose` logs the image header, target slot, every link frame and
every `UpdateData` chunk with its offset, flash address, rate and a data
preview, then the verify timing and throughput.

The FlatBuffers Python runtime and the generated `hwlink` package are
vendored in `vendor/` and `generated/` so the updater does not depend on a
`flatc` installation.  Regenerate them after changing `protocol/hwlink.fbs`:

```sh
flatc --python -o tools/generated protocol/hwlink.fbs
```

## Flash GUI

`flash_gui.py` is a PySide6 front-end for the host tools.  It is a host-side
app with a sidebar and two modes:

**Bootloader** — program BOOT over SWD.  By default OpenOCD runs inside the
`stm32-dev` container through `scripts/docker-dev.sh`, so no host toolchain
is needed; turn the container switch off to use the host's OpenOCD/st-info.
Pick `build/boot.elf` (or a `.hex`/`.bin`) and press **Flash**; **Erase**,
**Reset** and **Probe** use the same helper.

**Application** — update an application image over the ST-LINK VCP with
HWLINK (`bootctl.py update --json`).  Pick a `.img` (created by
`mkimage.py`), the serial port and baud.  The target slot is read-only and
comes from BOOT's `target_slot` (protocol minor 2); BOOT decides.  **Enter
BOOT** asks a running application to reset into BOOT, and update/info do it
automatically.  **Activate after update** appends `--activate --wait`, so
the GUI waits for the application's `GetStatus` to report `CONFIRMED` or a
rollback.  The log and progress bar are fed by `bootctl --json` events.

The **Production** page runs the ordered workflow: build BOOT, build the
application, provision option bytes, flash BOOT, flash slots A+B, verify.

```sh
make gui-deps       # one-time: install PySide6 and pyserial
make gui            # launch the GUI
```

Firmware can also be dragged onto the window.  Shortcuts: `Ctrl+O` browse,
`Ctrl+Enter` flash, `Ctrl+R` reset, `Esc` cancel.  The last firmware,
address, port, production paths and slot choices are remembered between
sessions.

`make gui-check` byte-compiles the app and runs as part of `make build`, but
PySide6 is only needed to run the GUI.
