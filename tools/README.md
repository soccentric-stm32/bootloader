# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header; `--inspect` checks an image file. |
| `bootctl.py` | HWLINK command-line updater: `info`, `update`, `activate`, `reset`, `set-baud`. |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |

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
