# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header; `--inspect` checks an image file. |
| `bootctl.py` | HWLINK command-line updater: `info`, `update`, `activate`, `reset`, `set-baud`. |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |
| `flash_gui.py` | PySide6 GUI for flash, erase, reset and probe over SWD (wraps `docker-dev.sh`). |

`bootctl.py` needs only pyserial:

```sh
pip install -r tools/requirements.txt
tools/bootctl.py --port /dev/ttyACM0 info
```

The FlatBuffers Python runtime and the generated `hwlink` package are
vendored in `vendor/` and `generated/` so the updater does not depend on a
`flatc` installation.  Regenerate them after changing `protocol/hwlink.fbs`:

```sh
flatc --python -o tools/generated protocol/hwlink.fbs
```

## Flash GUI

`flash_gui.py` is a minimal PySide6 front-end for the SWD commands.  It is a
host-side app: by default it runs OpenOCD inside the `stm32-dev` container
through `scripts/docker-dev.sh`, so no host toolchain is needed.  Clear the
container switch to use the host's OpenOCD/st-info instead.

```sh
make gui-deps       # one-time: install PySide6
make gui            # launch the GUI
```

Pick `build/boot.elf` (or a `.hex`/`.bin`), then press **Flash**; **Erase**,
**Reset** and **Probe** use the same helper.  Firmware can also be dragged
onto the window.  Shortcuts: `Ctrl+O` browse, `Ctrl+Enter` flash, `Ctrl+R`
reset, `Esc` cancel.  The last firmware, address and container choice are
remembered between sessions.

`make gui-check` byte-compiles the app and runs as part of `make build`, but
PySide6 is only needed to run the GUI.
