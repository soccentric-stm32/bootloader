# Improvements

Review of the BOOT firmware, `tools/bootctl.py`, `tools/flash_gui.py` and
the C application's boot contract, with the goal of one tool that builds
BOOT and the application, flashes BOOT over SWD and puts the application in
both slot A and slot B.

Author: Sandesh Ghimire <sandesh@soccentric.com>
Date:   2026-11-04

Priorities: **P0** can lose the known-good image or boot the wrong one,
**P1** blocks the build / flash / A+B workflow, **P2** robustness and
usability, **P3** cleanup and long-term.

---

## 1. Firmware

### P0-1 An update during a trial boot erases the rollback image

`boot_flash_target_slot()` always returns the slot BOOT is not running.  If
the running slot is pending (activated, not yet `CONFIRMED`), the other slot
is the last known-good image.  `UpdateBegin` erases its metadata sector
(`src/app/update.c:138`), so a failed trial has nothing to roll back to.

Fix: `boot_update_begin()` returns `BAD_STATE` ("active slot is on trial")
while `selection->active_pending` is set.  Optionally add a `force` flag to
`UpdateBegin` for bench use.  Same check in `boot_update_activate()`.

### P0-2 `GetBootInfo` undoes an in-RAM rejection

When the `REJECTED` record cannot be programmed, `main.c:147` rejects the
slot in RAM only.  `send_boot_info()` (`src/protocol/proto.c:272`) rescans
flash and calls `boot_flash_set_active()`, so the exhausted slot becomes
active again and the next update targets the good slot.

Fix: keep a `ram_rejected` mask in `boot_selection_t` that
`boot_selection_scan()` re-applies, or have `send_boot_info()` refresh slot
details without changing the active/target selection while an update
session is open.

### P0-3 No way to tell "no bootable image" from "slot A active"

With no valid slot, BootInfo reports `active_slot = A` and BOOT writes slot
A.  Hosts infer "target = B" and the GUI disables slot A.

Fix: add `active_valid:bool` and `target_slot:Slot` to `BootInfo` in
`protocol/hwlink.fbs` (protocol minor 2).  Hosts must use `target_slot`
instead of guessing.

### P1-4 Factory provisioning of both slots

Writing only the inactive slot is correct for field updates, but in the
factory it means four round trips per board.  Add a way to pre-build the
metadata sectors on the host (see 2.4) so SWD can write BOOT, slot A, slot
B and both metadata sectors in one pass.  BOOT needs no change for this if
the records follow Appendix B exactly; add a layout test for it in CI.

### P2-5 Activate reply can be lost

`handle_activate()` sends the Ack and calls `NVIC_SystemReset()` at once.
If the Ack is lost, the host retries into a reset board and reports a
failure even though activation succeeded.  Wait for UART TX complete (or a
short fixed delay) before the reset, and have hosts treat a timeout after
Activate as "check with `info`" instead of an error.

### P2-6 Trial counter is lost on power loss (OPEN-B03)

The counter lives in `RTC_BKP0R`; VBAT is tied to VDD, so a power cycle
resets it and a bad image that hangs before confirming is retried forever
across power cycles.  Option: add an append-only `TRIAL` record area in the
metadata sector (one 32-byte record per attempt, e.g. 8 slots at
`0x180..0x27F`) and count programmed records.  Costs one flash word write
per pending boot, none once confirmed.

### P2-7 Boot time: full CRC of both slots on every reset

`boot_selection_scan()` CRCs both slots (up to 976 KB each) on every boot,
and again on every `GetBootInfo`.  Measure it; if it matters, CRC only the
candidate slot first and the fallback only when the candidate fails, and
cache scan results inside an update session.

### P2-8 Image authentication (BSAFE-006)

CRC32 only protects against corruption.  When signing is in scope: ECDSA
P-256 or Ed25519 signature in the 256-byte header's reserved area, public
key in BOOT flash, verify at `UpdateEnd` and at boot.  Add a monotonic
minimum version (anti-rollback) at the same time.

### P3-9 Smaller items

- `src/protocol/proto.c:726`: comment says the IWDG survives a reset; with
  the software watchdog it does not.  Fix the comment.
- `ld/boot.ld`: assert on `_ebss + _Min_Stack_Size` explicitly rather than
  relying on the `._user_heap_stack` overflow error.
- `boot_wrong_flash_mode()` kicks an IWDG that was never started; harmless,
  but drop it or start the IWDG consistently.
- Consider a `GetBootInfo` field with BOOT's own version and build so hosts
  can detect an outdated bootloader.

---

## 2. Host tools

### P1-1 `bootctl update` does not reach BOOT from a running application

The running application answers `Hello` with role `APPLICATION`, so
`update`, `info` and `activate` fail.  BOOT only listens for 500 ms after
reset and nothing resets the board.

Fix: in `Link.hello()` (or a new `ensure_boot()`), if the role is
`APPLICATION`, send `EnterBootloader`, close and reopen the port, and retry
`Hello` for a few seconds.  Expose `--no-enter` for the old behaviour.

### P1-2 Flash both slots

Add `bootctl.py update IMAGE --both` (and a GUI action) that runs:

1. update + activate the inactive slot;
2. wait for the application, poll `GetStatus` / health until the slot is
   `CONFIRMED` (timeout, report rollback if it happens);
3. `EnterBootloader`, update + activate the other slot;
4. wait for confirm again, print final `info`.

Accept two different images (`--a IMG --b IMG`) for A/B testing.

### P1-3 Use the device's target slot

`print_boot_target()` and the GUI guess "target = not active".  Use
`target_slot` from P0-3 and show it read-only.  Remove the A/B radio
buttons from the GUI (BOOT decides; the choice is cosmetic today), or keep
them as an assertion: refuse to start if the device's target differs.

### P1-4 Factory images: `mkimage.py --metadata`

Generate a 16 KB metadata sector blob (header + `VERIFIED` + `ACTIVATED`
seq N + `CONFIRMED`) for a given slot, and a combined `.hex` with BOOT,
both slots and both metadata sectors at their physical addresses.  The GUI
and `provision.sh` then flash it with a single OpenOCD `program`.

### P2-5 Port discovery

`find_port()` and `_populate_ports()` glob `/dev/ttyACM*` only.  Use
`serial.tools.list_ports` and match the ST-LINK VID `0x0483`, so it works on
macOS and Windows and picks the right port when several are attached.

### P2-6 Timeouts and retries

- `UpdateData` default timeout is 10 s with one retry; fine for a 2 s sector
  erase, but the first chunk in each 128 KB sector should not print a
  timeout warning.  Show "erasing sector n" from the `Progress` events.
- After `Activate`, treat a timeout as success-pending and confirm with
  `info` (see 1, P2-5).

### P2-7 Machine-readable output for every command

Only `info` has `--json`.  Add `--json` (one JSON object per line) to
`update`, `activate`, `enter`, so the GUI parses state and progress instead
of regex-matching percentages (the erase `Progress` events currently make
the GUI bar jump).

### P3-8 One copy of the shared code

`mkimage.py` is copied into three application repos and `hwlink.fbs` has
already drifted (the C application's copy lacks `header_crc32`).  Publish
`hwlink.fbs`, the generated Python package and `mkimage.py` from one place
(a small `soccentric-hwlink` package or a git submodule) and check the
copies in CI.

---

## 3. GUI (`tools/flash_gui.py`)

### P1-1 One guided workflow

The GUI only flashes existing files.  Add a "Production" page with ordered
steps, each with status and log:

1. **Build BOOT**: `make build` (container or native).
2. **Build application**: select the C / Rust / Zephyr repo, run its build,
   then `mkimage.py`.
3. **Provision**: option bytes `nDBANK = 0`, `nDBOOT = 1` (the README says
   the GUI does this; it does not today) — reuse `tools/provision.sh`.
4. **Flash BOOT** over SWD.
5. **Flash application to A and B** (serial loop from 2, P1-2, or the
   factory hex from 2, P1-4).
6. **Verify**: `info` shows both slots valid and confirmed.

### P1-2 Enter-bootloader action

Add an "Enter BOOT" button and auto-enter before update / info (2, P1-1).
Today boot info silently fails while the application runs, and the "active
slot" guard is skipped.

### P2-3 State after activation

After `--activate` the board resets into the application; the GUI should
wait for the application's `HelloReply`, show the trial state and report
confirm or rollback, rather than ending at "Done".

### P2-4 Erase in native mode

`flash erase_sector 0 0 last` may cover only bank 1 depending on how the
OpenOCD `stm32f2x` driver maps the dual-bank F767.  Verify on hardware;
otherwise use `stm32f2x mass_erase 0`.

### P2-5 Container and paths

Container mode refuses files outside the bootloader repo, so an application
built in a sibling repo cannot be used there.  Mount the workspace root (or
the selected file's directory) in `docker-dev.sh`.

### P3-6 Structure

At ~1300 lines the window class owns UI, process control and protocol
state.  Split into `views/`, a `Runner` (QProcess queue for multi-step
jobs) and a `Device` model fed by `bootctl --json`.

---

## 4. Testing and CI

- **Host unit tests** for `src/common` and `src/app/selection.c` /
  `update.c` (compile for x86 with a fake flash and fake backup registers):
  torn records, sequence saturation, trial limit, update-during-trial
  (P0-1), rescan after RAM rejection (P0-2).
- **Python tests** for `bootctl.py` against a simulated BOOT (pty + the same
  state machine in Python): resend with the same seq, lost Ack, out-of-order
  resync, `--both` loop.
- **Hardware-in-the-loop job** on a bench NUCLEO: provision, flash BOOT,
  update A, confirm, update B, force a bad image and check rollback, power
  cycle during an update.
- **Cross-repo contract check** in CI: compare `boot_layout.h` /
  `selection.h` constants with each application's `boot_contract.h` and
  `memory.x`, and the `hwlink.fbs` copies.

---

## Suggested order

1. P0-1, P0-2, P0-3 in firmware, with host unit tests.
2. bootctl auto-enter, `target_slot`, `--json` everywhere.
3. `--both` loop and `mkimage.py --metadata` / factory hex.
4. GUI guided workflow on top of 2 and 3.
5. Trial counter in flash, then signing and anti-rollback.
