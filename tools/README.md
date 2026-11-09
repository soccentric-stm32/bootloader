# Host tools

| Tool | Purpose |
|---|---|
| `mkimage.py` | Wrap an application `.bin` in the 256-byte BOOT image header (`--key` signs it, BSAFE-006); `--sign` signs an existing image; `--inspect [--key]` checks an image file and its signature; `--genkey` and `--export-pubkey` manage signing keys; `--metadata` builds a 16 KB metadata sector and `--factory-hex` combines BOOT + A/B + metadata (P1-4). |
| `bootctl.py` | HWLINK command-line updater: `info`, `update` (`--both`, `--a/--b`, `--json`), `activate`, `enter`, `wait`, `catch`, `reset`, `set-baud`. |
| `check_contracts.py` | Cross-check layout constants, generated schema and an application repo's `boot_contract.h`/`hwlink.fbs`. |
| `package_hwlink.py` | Publish the canonical `hwlink.fbs`, generated Python package, `mkimage.py`, `boot_layout.h` and vendored runtime bundle (P3-8). |
| `provision.sh` | Set dual-bank option bytes, mass-erase and flash BOOT over SWD with OpenOCD. |

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

The BOOT Flasher GUI lives in the
[bootloader-gui](https://github.com/soccentric-stm32/bootloader-gui)
repository.  It drives `bootctl.py`, `mkimage.py`, `provision.sh`,
`scripts/docker-dev.sh` and `keys/` from this repository, expected as a
sibling checkout (`BOOTLOADER_DIR` points at another one).
