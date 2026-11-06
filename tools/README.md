# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header (`--key` signs it, BSAFE-006); `--sign` signs an existing image; `--inspect [--key]` checks an image file and its signature; `--genkey` and `--export-pubkey` manage signing keys; `--metadata` builds a 16 KB metadata sector and `--factory-hex` combines BOOT + A/B + metadata (P1-4). |
| `bootctl.py` | HWLINK command-line updater: `info`, `update` (`--both`, `--a/--b`, `--json`), `activate`, `enter`, `wait`, `reset`, `set-baud`. |
| `check_contracts.py` | Cross-check layout constants, generated schema and an application repo's `boot_contract.h`/`hwlink.fbs`. |
| `package_hwlink.py` | Publish the canonical `hwlink.fbs`, generated Python package, `mkimage.py`, `boot_layout.h` and vendored runtime bundle (P3-8). |
| `gui/` | The flash GUI: `window.py` (main window), `pages.py` (bootloader and project pages), `projects.py` (the five firmware projects), `runner.py` (job queue), `device.py` (BootInfo, port discovery), `widgets.py`, `theme.py`, `icons.py`. |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |
| `flash_gui.py` | BOOT Flasher, the PySide6 GUI: build and flash BOOT over SWD; clone, build, sign and flash the five firmware projects through BOOT. |

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

`flash_gui.py` (BOOT Flasher) does two things: flash the bootloader, and
flash the five firmware projects.  The sidebar has one page for each; its
**Board** panel shows the ST-LINK port and what BOOT reports (version,
active and next slot, trusted key, security floor).  The panel reads with
`bootctl --no-enter`, so it never stops a running application.

**Bootloader** builds BOOT (`make build`) and programs it over SWD.  By
default both run in the `stm32-dev` container through
`scripts/docker-dev.sh`; turn **Build in Docker** off to use the host's
cmake and OpenOCD.  **More** has Probe, Reset, Provision (option bytes,
mass erase, BOOT) and Erase; the last two ask first.

**Applications**: app-c-freertos, app-c-zephyr, app-rust-freertos,
app-rust-zephyr and validation-firmware.  **Build, Sign & Flash** runs:

1. **Clone**, when the repository is missing: `git clone` with the URL from
   the workspace `repos.yaml`, next to this repository.
2. **Build**: the project's `make build`.
3. **Sign**: `mkimage.py --sign` into `<image>-signed.img`, then
   `--inspect` (BSAFE-006).  *Match board* signs with BOOT's floor; if the
   floor is unknown the step reads it first.
4. **Flash**: `bootctl.py update --activate --wait`.

Before flashing, the page warns if the key differs from the one BOOT
trusts, or if the counter is below the floor or would raise it.  Each step
also runs on its own.  **More** has Pull, Open folder, Enter bootloader and
Reset.

```sh
make gui-deps       # one-time: PySide6, pyserial, cryptography
make gui            # launch the GUI
```

Shortcuts: `Ctrl+1`..`Ctrl+6` pages, `Ctrl+Enter` run the whole pipeline,
`Ctrl+B` build, `Ctrl+Shift+Enter` flash only, `Esc` cancel, `F5` refresh
the board, `Ctrl+R` reset, `Ctrl+K` clear the output.  The GUI follows the
system light/dark setting.  Commands, paths, keys and counters are
remembered per page.  `FIRMWARE_WORKSPACE` points at another workspace
directory.

`make gui-check` byte-compiles the app and runs as part of `make build`, but
PySide6 is only needed to run the GUI.
