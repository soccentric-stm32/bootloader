# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header; `--inspect` checks an image file. |
| `bootctl.py` | HWLINK command-line updater: `info`, `update`, `activate`, `reset`, `set-baud`. |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |
| `flash_gui.py` | PySide6 GUI: flash BOOT over SWD, or update an application image over HWLINK with a slot suggestion. |

`bootctl.py` needs only pyserial:

```sh
pip install -r tools/requirements.txt
tools/bootctl.py --port /dev/ttyACM0 info
tools/bootctl.py --port /dev/ttyACM0 info --json   # machine-readable
tools/bootctl.py --port /dev/ttyACM0 -v update build/app.img --activate
```

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
HWLINK (`bootctl.py update`).  Pick a `.img` (created by `mkimage.py`), the
serial port and baud.  The GUI reads the boot info and preselects the
**alternate** slot: BOOT only writes the inactive slot (UPD-001), so the
active slot is disabled and the hint shows which slot will be written.
**Activate after update** appends `--activate`.  **Verbose output** (on by
default) passes `-v` to `bootctl.py`, so the log shows every chunk with its
offset, flash address, rate and data preview; the progress bar follows the
upload.  The **Boot info** button re-reads slots, version and flash mode.

```sh
make gui-deps       # one-time: install PySide6 and pyserial
make gui            # launch the GUI
```

Firmware can also be dragged onto the window.  Shortcuts: `Ctrl+O` browse,
`Ctrl+Enter` flash, `Ctrl+R` reset, `Esc` cancel.  The last firmware,
address, port and slot choices are remembered between sessions.

`make gui-check` byte-compiles the app and runs as part of `make build`, but
PySide6 is only needed to run the GUI.
