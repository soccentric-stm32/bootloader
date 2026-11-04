# Software Requirements Specification — NUCLEO-F767ZI A/B Bootloader

| | |
|---|---|
| **Document** | SRS-BOOT-F767 |
| **Version** | 0.1 (draft) |
| **Author** | Sandesh Ghimire <sandesh@soccentric.com> |
| **Date** | 2026-11-03 |
| **Status** | Draft for review |
| **Hardware** | NUCLEO-F767ZI, STM32F767ZIT6 rev Z (see [soccentric-stm32-validation-firmware/Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md)) |
| **Software base** | HAL/LL from STM32CubeF7 v1.17.4 as shipped; bare metal (no RTOS); flatcc runtime for FlatBuffers |

---

## Contents

1. [Introduction](#1-introduction)
2. [Overall description](#2-overall-description)
3. [Flash layout](#3-flash-layout)
4. [General requirements](#4-general-requirements)
5. [Image format](#5-image-format)
6. [Boot state and bank selection](#6-boot-state-and-bank-selection)
7. [Trial boot and rollback](#7-trial-boot-and-rollback)
8. [Entering update mode](#8-entering-update-mode)
9. [Update protocol](#9-update-protocol)
10. [Hand-over to the application](#10-hand-over-to-the-application)
11. [Application contract](#11-application-contract)
12. [Safety and protection requirements](#12-safety-and-protection-requirements)
13. [Non-functional requirements](#13-non-functional-requirements)
14. [Host-side tooling requirements](#14-host-side-tooling-requirements)
15. [Verification matrix](#15-verification-matrix)
- [Appendix A — Boot flow](#appendix-a--boot-flow)
- [Appendix B — Metadata record format](#appendix-b--metadata-record-format)
- [Appendix C — Open items](#appendix-c--open-items)

---

## 1. Introduction

### 1.1 Purpose

This document specifies **BOOT**, a bare-metal A/B bootloader for the
NUCLEO-F767ZI. BOOT keeps two application slots, one per flash bank, receives
new application images over UART from the host GUI (SRS-GUI-F767), boots
new images on trial, and rolls back to the previous image when a new one does
not confirm itself.

### 1.2 Scope

BOOT shall:

- run the flash in dual-bank mode and use one bank per application slot;
- receive application images over the ST-LINK VCP using the HWLINK protocol;
- check every image with CRC32 before it is allowed to run;
- boot a new image on trial and fall back to the last confirmed image when the
  trial fails;
- hand over to the application with the MCU in a defined state.

BOOT does not cover image authentication (signing), encryption, anti-rollback,
updating itself in the field, or updates over USB, Ethernet or SD card.
HWVAL (SRS-HWVAL-F767) is not managed by BOOT; it is flashed over SWD and
replaces BOOT and both slots (§2.5).

### 1.3 Definitions

| Term | Meaning |
|---|---|
| **BOOT** | The bootloader specified here |
| **Application (app)** | A firmware image that runs under BOOT, e.g. the demo app (SRS-DEMO-F767) |
| **Bank 1 / Bank 2** | The two physical 1 MB flash banks in dual-bank mode |
| **Slot** | The application area of one bank (§3) |
| **Active slot** | The slot BOOT starts at the next boot |
| **Inactive slot** | The other slot; the only one BOOT writes during an update |
| **Image file** | Image header + application binary, produced by the host tool (§14) |
| **Pending** | An image that was activated but has not yet confirmed itself |
| **Confirmed** | An image that has reported itself healthy (§11) |
| **Rejected** | An image that failed its trial and will not be booted again |
| **Bank swap** | `SYSCFG_MEMRMP.SWP_FB`: maps bank 2 at `0x0800 0000` and bank 1 at `0x0810 0000` |
| **HWLINK** | The UART framing and FlatBuffers protocol shared by BOOT, HWVAL, the demo app and the GUI ([LINK]) |
| **Backup register** | `RTC_BKPxR`, survives system reset; lost on power loss (VBAT is tied to VDD on this board) |

### 1.4 Requirement conventions

- **shall** = mandatory, **should** = recommended, **may** = optional.
- Each requirement has a unique ID: `<AREA>-<NNN>`.
- Each requirement states a **verification method**:

| Method | Meaning |
|---|---|
| **T** | Test — executed on the board or by a host script with a pass/fail outcome |
| **D** | Demonstration — shown working, observed by the operator |
| **I** | Inspection — of source code, build output or documentation |
| **A** | Analysis — by calculation or review of measurements |

### 1.5 References

| ID | Document |
|---|---|
| [HW] | [soccentric-stm32-validation-firmware/Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) — board reference |
| [HWVAL] | [soccentric-stm32-validation-firmware/SRS.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/SRS.md) — SRS-HWVAL-F767, hardware validation firmware |
| [DEMO] | [soccentric-stm32-application/SRS.md](https://github.com/soccentric-stm32/soccentric-stm32-application/blob/HEAD/SRS.md) — SRS-DEMO-F767, demo application |
| [GUI] | [soccentric-stm32-validation-gui/SRS.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui/blob/HEAD/SRS.md) — SRS-GUI-F767, host GUI |
| [LINK] | [soccentric-stm32-validation-gui/SRS.md, Appendix A](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui/blob/HEAD/SRS.md#appendix-a--hwlink-protocol) — HWLINK protocol (normative) |
| [RM0410] | ST, STM32F76xxx/F77xxx reference manual (flash, dual-bank, SYSCFG, CRC) |
| [DS11532] | ST, STM32F767 datasheet (flash timings) |
| [UM1974] | ST, STM32 Nucleo-144 boards user manual |
| [ES0334] | ST, STM32F76x/F77x errata sheet (revision Z) |
| [FB] | FlatBuffers format and the flatcc C runtime |

---

## 2. Overall description

### 2.1 Product perspective

```
 Host PC                                   NUCLEO-F767ZI (dual-bank flash)
+--------------------+   ST-LINK VCP   +--------------------------------------+
| HWVAL GUI          |<===============>| USART3 (PD8/PD9)                     |
|  firmware update   |  HWLINK frames  |                                      |
|  page              |                 |  Bank 1                Bank 2        |
|                    |                 |  +----------------+   +------------+ |
| OpenOCD (SWD) ---->|-- SWD --------->|  | BOOT  (32 KB)  |   | reserved   | |
|  provisioning only |                 |  | META A (16 KB) |   | META B     | |
+--------------------+                 |  | SLOT A (976 KB)|   | SLOT B     | |
                                       |  +----------------+   +------------+ |
                                       +--------------------------------------+
```

### 2.2 Users

| User | Use |
|---|---|
| Firmware developer | Loads new app builds without a debugger; tests rollback |
| Test technician | Updates boards from the GUI |
| GUI / host script | Automated updates through HWLINK |

### 2.3 Constraints

| ID | Constraint |
|---|---|
| BCON-001 | BOOT shall be built with the arm-none-eabi GCC toolchain in the project Docker image, with one CMake command, like HWVAL. |
| BCON-002 | BOOT shall use HAL/LL from STM32CubeF7 v1.17.4 unmodified. Third-party code (flatcc runtime) shall be used as released, in project-owned build files. |
| BCON-003 | BOOT shall be bare metal: no RTOS, no dynamic memory allocation (`malloc`/`free` shall not be linked). |
| BCON-004 | BOOT shall use USART3 on the ST-LINK VCP (PD8 TX, PD9 RX) as its only update interface. |
| BCON-005 | BOOT shall fit in 32 KB (bank 1 sectors 0–1). |
| BCON-006 | BOOT shall run from internal flash and internal RAM only. |

### 2.4 Assumptions

| ID | Assumption |
|---|---|
| BASM-001 | The option bytes have been set once by provisioning (§14) to dual-bank mode (`nDBANK = 0`) and dual boot disabled (`nDBOOT = 1`); all other option bytes are at the measured defaults [HW §5.2]. |
| BASM-002 | `BOOT_ADD0` is at its default (`0x0080`, flash via ITCM, i.e. bank 1 start) and BOOT0 is low, so the MCU always starts BOOT in bank 1 sector 0 after reset. |
| BASM-003 | The board is powered from the ST-LINK USB port. VBAT is tied to VDD, so backup registers survive resets but not power loss. |
| BASM-004 | The ST-LINK VCP works in both directions (HWVAL CL-010: the board→host solder bridge must be closed). |

### 2.5 Relation to HWVAL

HWVAL requires single-bank mode (HWVAL ASM-003) and uses the whole flash. The
two configurations are mutually exclusive:

| Configuration | Option bytes | Flash contents | Loaded by |
|---|---|---|---|
| **Bench (HWVAL)** | `nDBANK = 1` | HWVAL in sectors 0–10, sector 11 free | GUI over SWD (OpenOCD) |
| **Product (BOOT)** | `nDBANK = 0` | BOOT + two slots | BOOT over SWD, then apps over UART |

Switching between them is done only by the GUI's SWD provisioning
([GUI] §10), never by BOOT or an application.

---

## 3. Flash layout

### 3.1 Dual-bank sector map

Each 1 MB bank has 4 × 16 KB, 1 × 64 KB and 7 × 128 KB sectors [HW §5.1].
Addresses below are the AXIM addresses **without bank swap**.

| Region | Bank 1 sectors | Bank 1 address | Bank 2 sectors | Bank 2 address | Size |
|---|---|---|---|---|---|
| BOOT | 0–1 | `0x0800 0000`–`0x0800 7FFF` | — | — | 32 KB |
| Reserved | — | — | 0–1 | `0x0810 0000`–`0x0810 7FFF` | 32 KB |
| Metadata | 2 | `0x0800 8000`–`0x0800 BFFF` | 2 | `0x0810 8000`–`0x0810 BFFF` | 16 KB |
| Slot | 3–11 | `0x0800 C000`–`0x080F FFFF` | 3–11 | `0x0810 C000`–`0x081F FFFF` | 976 KB |

Slot A is in bank 1, slot B in bank 2. Bank 2 sectors 0–1 are reserved for a
future recovery copy of BOOT (out of scope for this version) and shall stay
erased.

### 3.2 Layout requirements

| ID | Requirement | Method |
|---|---|---|
| LAY-001 | BOOT shall be linked at `0x0800 0000` and the build shall fail if it exceeds `0x0800 7FFF`. | I |
| LAY-002 | Every application shall be linked with its vector table at `0x0800 C000` and shall not exceed `0x080F FFFF` (976 KB). Because of the bank swap (§6.3) the same binary runs from either slot. | I |
| LAY-003 | Each bank's sector 2 shall hold the metadata of that bank's slot only (Appendix B). | I |
| LAY-004 | BOOT shall never erase or program bank 1 sectors 0–1 (its own code) or bank 2 sectors 0–1. The flash wrapper shall reject these addresses. | I, T |
| LAY-005 | The layout constants (sector numbers, addresses, sizes) shall be defined in one header shared by BOOT, the applications and the host image tool. | I |

---

## 4. General requirements

| ID | Requirement | Method |
|---|---|---|
| BGEN-001 | The build shall produce `boot.elf`, `boot.bin`, `boot.hex` and `boot.map`. | I |
| BGEN-002 | BOOT shall embed a version string (semantic version, git commit, build date) and report it in the HWLINK `Hello` reply. | T |
| BGEN-003 | BOOT shall run from the 8 MHz HSE bypass clock through the PLL at a frequency chosen so that USART3 meets BGEN-004. The chosen clock tree shall be documented. | I, T |
| BGEN-004 | USART3 shall support 115200 baud (default) and 921600 baud with a baud-rate error of at most 1.5 %, 8N1, no flow control. | A, T |
| BGEN-005 | BOOT shall use the hardware CRC unit for all CRC32 calculations, configured for CRC-32/ISO-HDLC (polynomial `0x04C11DB7`, init `0xFFFFFFFF`, reflected input and output, final XOR `0xFFFFFFFF`) so that results equal Python's `zlib.crc32`. | T |
| BGEN-006 | BOOT shall install a HardFault handler that blinks LD3 (red) in a fixed fault pattern and resets after 5 s. | T |
| BGEN-007 | BOOT shall not clear the reset flags in `RCC_CSR`; it may read them. The application owns reporting and clearing them. | T |
| BGEN-008 | BOOT shall show its state on the LEDs: LD2 (blue) on while BOOT runs, LD2 fast blink while an update is in progress, LD3 (red) on while no bootable image exists. All LEDs shall be off at hand-over. | D |
| BGEN-009 | All source files shall carry the author header of the project. | I |
| BGEN-010 | BOOT shall use polling or interrupts with a fixed-size receive ring buffer of at least 4 KB; no frame byte shall be lost at 921600 baud while flash is being programmed. | T |

---

## 5. Image format

### 5.1 Image file

An image file is a 256-byte header followed by the application binary
(`.bin`, starting with the vector table linked at `0x0800 C000`).

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `magic` | `0x48574249` (`"IBWH"` little-endian) |
| 4 | 2 | `header_version` | Format version of this header, `1` |
| 6 | 2 | `header_size` | `256` |
| 8 | 4 | `image_type` | `1` = application (other values reserved) |
| 12 | 4 | `version` | Semantic version, `major << 24 \| minor << 16 \| patch` |
| 16 | 4 | `image_size` | Size of the binary in bytes, excluding the header |
| 20 | 4 | `image_crc32` | CRC32 of the binary |
| 24 | 4 | `load_address` | `0x0800 C000` |
| 28 | 4 | `entry_address` | Reset handler address from the vector table |
| 32 | 4 | `build_timestamp` | Unix time of the build |
| 36 | 20 | `git_commit` | Git commit hash (binary SHA-1) |
| 56 | 32 | `name` | Zero-padded ASCII product name, e.g. `"stm32-application"` |
| 88 | 164 | `reserved` | Zero; reserved for a future signature |
| 252 | 4 | `header_crc32` | CRC32 of bytes 0–251 |

### 5.2 Image requirements

| ID | Requirement | Method |
|---|---|---|
| IMG-001 | BOOT shall reject an image whose `magic`, `header_version`, `header_size`, `image_type` or `header_crc32` is wrong, with error `BAD_HEADER`. | T |
| IMG-002 | BOOT shall reject an image whose `load_address` is not `0x0800 C000` or whose `image_size` exceeds 976 KB, with error `BAD_LAYOUT`. | T |
| IMG-003 | BOOT shall reject an image whose initial stack pointer (first vector word) is not inside internal RAM, or whose reset vector is not inside the slot, with error `BAD_VECTORS`. | T |
| IMG-004 | BOOT shall check `image_crc32` over the programmed slot contents (read back from flash, not from the receive buffer) before an image can be activated. | T |
| IMG-005 | BOOT shall check `image_crc32` of the selected slot again on every boot before handing over (§6.2). | T |
| IMG-006 | BOOT shall store the header unchanged in the slot's metadata sector, so that `GetBootInfo` can report the version, build time, commit and name of both slots. | T |
| IMG-007 | BOOT shall accept any `version`, including one older than the other slot (no anti-rollback). The GUI warns about downgrades ([GUI] FW-008). | T |

---

## 6. Boot state and bank selection

### 6.1 Where state is kept

| State | Storage | Survives |
|---|---|---|
| Image header, activation sequence number, slot state (pending/confirmed/rejected) | Metadata sector of the slot's bank, append-only records (Appendix B) | Reset and power loss |
| Trial boot counter, slot under trial | Backup register `RTC_BKP0R` (magic + slot + count) | Reset only |
| "Stay in BOOT" request from the app | Backup register `RTC_BKP1R` (magic word) | Reset only |

Note: the bank swap bit `SWP_FB` is volatile (it resets to 0); BOOT sets it on
every boot from the metadata. No option byte is written by BOOT or by an
application to switch slots.

#### 6.1.1 Backup-register layout (normative)

These words are the shared BOOT/application contract (`selection.h`,
`boot_contract.h`, the Rust `board.rs`). A change requires a protocol minor
bump and matching updates in every copy.

| Register | Meaning | Bits |
|---|---|---|
| `RTC_BKP0R` | Trial counter (BOOT) | `0..23` magic `TRI` (`0x0054_5249`); `24..25` slot (1=A, 2=B, 0=none); `26..31` trial count (mask `0x3F`) |
| `RTC_BKP1R` | "Stay in BOOT" request (application) | The whole word is `STAY` (`0x5941_5453`); any other value means no request |
| `RTC_BKP2R` | Hand-over record (BOOT → application) | `0..23` magic `HND` (`0x0044_4E48`); `24..25` slot; bit `26` trial pending; `27..28` trial count |
| `RTC_BKP3R` | Last boot outcome (BOOT) | `0..23` magic `OUT` (`0x004F_5554`); `24..27` outcome (0 unknown, 1 booted, 2 rolled back, 3 update mode); `28..29` rejected slot (0 none, 1 A, 2 B) |

#### 6.1.2 Reserved application no-init window (normative)

BOOT uses DTCM `0x2000_0000..0x2000_EFFF` (60 KB budget, including its stack)
and SRAM1 `0x2002_0000..0x2002_0FFF` (USART3 DMA ring). Applications place
their reset-surviving fault record in the reserved window
`0x2001_FC00..0x2001_FFFF` (top 1 KB of DTCM) and start their main stack below
it. BOOT never reads or writes that window; the applications assert the
address and size in their linker scripts.

### 6.2 Selection

| ID | Requirement | Method |
|---|---|---|
| SEL-001 | A slot is **valid** when its metadata contains a header record that passes IMG-001…IMG-003, the slot contents pass the CRC check of IMG-005, and the slot has an `ACTIVATED` record and no `REJECTED` record. | T |
| SEL-002 | Among valid slots, BOOT shall boot the one with the highest activation sequence number (§9, `Activate`). | T |
| SEL-003 | When no slot is valid, BOOT shall stay in update mode indefinitely (LD3 on, BGEN-008). | T |
| SEL-004 | Sequence-number comparison shall handle the case where only one slot has ever been written, and shall treat erased (`0xFFFFFFFF`) sequence fields as invalid. | T |
| SEL-005 | BOOT shall complete selection (both CRC checks of 976 KB worst case included) within 500 ms at the BOOT clock. | A, T |

### 6.3 Bank swap

| ID | Requirement | Method |
|---|---|---|
| SWP-001 | To start slot A, BOOT shall leave `SWP_FB = 0`. To start slot B, BOOT shall set `SWP_FB = 1`, so that bank 2 appears at `0x0800 0000` and slot B at `0x0800 C000`. | T |
| SWP-002 | Because setting `SWP_FB` remaps the bank BOOT itself executes from, the swap and the jump to the application shall run from a routine copied to RAM (ITCM or DTCM), with interrupts disabled, followed by `DSB`/`ISB` and invalidation of the instruction cache and the ART accelerator. | I, T |
| SWP-003 | BOOT shall read and write both banks only through addresses computed from the current `SWP_FB` value (it is 0 while BOOT runs). The flash wrapper shall map slot letters to physical bank and sector numbers in one place. | I |

---

## 7. Trial boot and rollback

| ID | Requirement | Method |
|---|---|---|
| TRY-001 | After `Activate`, the newly written slot shall be **pending** until the application writes a `CONFIRMED` record ([§11](#11-application-contract)). | T |
| TRY-002 | Each time BOOT starts a pending slot it shall increment the trial counter in `RTC_BKP0R` before the hand-over. | T |
| TRY-003 | The trial limit shall be 3 boots (build-time constant). When BOOT finds a pending slot whose trial counter has reached the limit, it shall write a `REJECTED` record for that slot and select again (SEL-002), which boots the other slot. | T |
| TRY-004 | BOOT shall start the independent watchdog (IWDG) before handing over to any application, with a timeout of 4 s (build-time constant), so that a hung application resets and counts as a failed trial. | T |
| TRY-005 | The trial counter shall be the number of contiguous `TRIAL` records in the pending slot's metadata sector, so it survives a power loss. The RTC backup register is a same-session fallback when the metadata write fails (Appendix C, OPEN-B03). | T |
| TRY-006 | When the application writes `CONFIRMED`, the trial counter shall be cleared by the next boot. | T |
| TRY-007 | A rejected slot shall stay rejected until it is overwritten by a new update. | T |
| TRY-008 | When rollback happened, BOOT shall report it in `GetBootInfo` (last boot outcome `ROLLED_BACK`, rejected slot, reason `TRIAL_LIMIT`) until the next successful update. | T |

---

## 8. Entering update mode

| ID | Requirement | Method |
|---|---|---|
| ENT-001 | After reset, BOOT shall listen on USART3 for a HWLINK `Hello` frame for a listen window of 500 ms (build-time constant, 0–2000 ms) before booting the selected slot. | T |
| ENT-002 | BOOT shall stay in update mode when `RTC_BKP1R` holds the "stay in BOOT" magic (written by an application on `EnterBootloader`, [DEMO] COM-006). BOOT shall clear the magic as soon as it has read it. | T |
| ENT-003 | BOOT shall stay in update mode when the user button B1 (PC13) is held at reset. | D |
| ENT-004 | BOOT shall stay in update mode when no slot is valid (SEL-003). | T |
| ENT-005 | Once a `Hello` has been received, BOOT shall stay in update mode until it receives `Reset` or `Activate`, or until 120 s pass with no valid frame, after which it boots normally. | T |

---

## 9. Update protocol

BOOT implements the HWLINK framing and common messages of [LINK], and the
bootloader message set below. Message names refer to the FlatBuffers schema
`protocol/hwlink.fbs` ([LINK] A.5).

### 9.1 Messages

| Request (host → BOOT) | Response (BOOT → host) | Purpose |
|---|---|---|
| `Hello` | `HelloReply` (role `BOOTLOADER`, versions, UID, protocol version, max frame size) | Identify the firmware |
| `SetBaud` | `Ack`, then switch | Change baud rate (115200 / 921600) |
| `GetBootInfo` | `BootInfo` | State of both slots: header fields, valid/pending/confirmed/rejected, sequence numbers, trial count, active slot, last boot outcome |
| `UpdateBegin` (header) | `Ack` / `Nak` | Start writing the inactive slot |
| `UpdateData` (offset, bytes) | `Ack` (next expected offset) / `Nak` | Program one chunk |
| `UpdateEnd` | `UpdateResult` (CRC OK / error) | Verify the slot |
| `Activate` | `Ack`, then reset | Make the verified slot pending and reboot into it |
| `UpdateAbort` | `Ack` | Abandon the update; the inactive slot is left invalid |
| `Reset` | `Ack`, then reset | Leave BOOT and boot normally |

### 9.2 Requirements

| ID | Requirement | Method |
|---|---|---|
| UPD-001 | BOOT shall only write the **inactive** slot: the slot that is not currently selected by SEL-002. If no slot is valid, slot A shall be written. | T |
| UPD-002 | `UpdateBegin` shall check the header (IMG-001…IMG-002). On success BOOT shall erase the inactive bank's metadata sector and program the header record into it, then reply `Ack`. | T |
| UPD-003 | BOOT shall erase slot sectors lazily: a sector is erased when the first `UpdateData` chunk that falls into it arrives. The reply to that chunk may therefore take up to the maximum 128 KB sector erase time [DS11532] plus margin; BOOT shall document the worst case (Appendix C, OPEN-B02). | T, A |
| UPD-004 | `UpdateData` chunks shall carry an offset and 1 to 1024 bytes. BOOT shall accept only the next expected offset; a repeated chunk (same offset as the last acknowledged one) shall be acknowledged again without reprogramming; any other offset shall be answered with `Nak OUT_OF_ORDER` and the expected offset. | T |
| UPD-005 | BOOT shall program flash in 32-bit words, verify each programmed word by read-back, and answer `Nak FLASH_ERROR` with the address on any mismatch or flash error flag. | T |
| UPD-006 | `UpdateEnd` shall be accepted only after exactly `image_size` bytes; BOOT shall then run IMG-003 and IMG-004 and reply with `UpdateResult`. On success it shall program a `VERIFIED` record. | T |
| UPD-007 | `Activate` shall be accepted only for a `VERIFIED` slot. BOOT shall program an `ACTIVATED` record with sequence number = (highest sequence number of the other slot) + 1, set the trial counter to 0 for that slot, reply `Ack`, and reset. | T |
| UPD-008 | Losing power or the link at any point during an update shall leave the active slot untouched and bootable; the partly written slot shall be invalid (no `ACTIVATED` record). | T |
| UPD-009 | BOOT shall reply to every request within 100 ms, except UPD-003 sector erases and UPD-006 verification, which shall report progress with `Progress` events at least every 500 ms. | T |
| UPD-010 | Unknown or malformed frames shall be answered with `Nak` and an error code ([LINK] A.6), without changing state. | T |

---

## 10. Hand-over to the application

| ID | Requirement | Method |
|---|---|---|
| HND-001 | Before jumping, BOOT shall: disable all interrupts and SysTick; clear all pending NVIC interrupts; de-initialise USART3, GPIO and the CRC unit; return the clock tree to the reset state (HSI on, PLL off, SYSCLK = HSI, flash wait states 0); leave caches as at reset. | T |
| HND-002 | BOOT shall set `SCB->VTOR` to `0x0800 C000`, load MSP from the first vector word, and branch to the reset vector (SWP-002 for slot B). | T |
| HND-003 | BOOT shall pass a hand-over record to the application in backup register `RTC_BKP2R`: magic, booted slot (A/B), trial state (confirmed or pending + count). | T |
| HND-004 | The IWDG shall be running at hand-over (TRY-004). | T |
| HND-005 | Time from reset to hand-over with no update request shall be at most listen window (ENT-001) + 500 ms. | T |

---

## 11. Application contract

Every application that runs under BOOT shall meet these requirements. They are
repeated in each application SRS (e.g. [DEMO] §4).

| ID | Requirement | Method |
|---|---|---|
| APP-001 | Link its vector table at `0x0800 C000` and fit in 976 KB (LAY-002). | I |
| APP-002 | Be packaged into an image file by the host image tool (§14). | I |
| APP-003 | Configure its own clocks from the reset state (HND-001). | I |
| APP-004 | Refresh the IWDG within its 4 s timeout from the first instruction on (HND-004). | T |
| APP-005 | Read `RTC_BKP2R` and, when its slot is pending, run its health check and then write a `CONFIRMED` record into its own metadata sector (now at `0x0800 8000` because of the swap) using the shared flash helper (LAY-005). It shall never erase that sector. | T |
| APP-006 | Implement HWLINK `Hello` (with its own role) and `EnterBootloader`: write the "stay in BOOT" magic to `RTC_BKP1R`, reply `Ack`, and reset. | T |
| APP-007 | Never write option bytes, bank 1 sectors 0–1, or the other bank. | I |

---

## 12. Safety and protection requirements

| ID | Requirement | Method |
|---|---|---|
| BSAFE-001 | BOOT shall never write option bytes. Code that unlocks the option bytes (`OPTKEYR`) shall not be compiled in. | I |
| BSAFE-002 | BOOT shall never write OTP or set read or write protection. | I |
| BSAFE-003 | The flash wrapper shall allow erase and program only in the inactive slot and the inactive bank's metadata sector, with one exception: the `CONFIRMED`/`REJECTED` records may be programmed into either metadata sector. Every other address shall be rejected. | I, T |
| BSAFE-004 | Metadata records shall be append-only: a word that has been programmed shall never be programmed again (Appendix B). | I |
| BSAFE-005 | BOOT shall check `nDBANK = 0` at start-up. If the flash is in single-bank mode it shall not write flash at all, report `WRONG_FLASH_MODE` in `HelloReply`, and blink LD3. | T |
| BSAFE-006 | Integrity is protected by CRC32 only. BOOT does not authenticate images; anyone with UART access can load an image. This limitation shall be stated in the user documentation. | I |

---

## 13. Non-functional requirements

| ID | Requirement | Method |
|---|---|---|
| BNFR-001 | **Footprint:** BOOT shall fit in 32 KB of flash and use at most 64 KB of RAM. The build shall report both. | I |
| BNFR-002 | **Update time:** a 976 KB image shall be written and verified in at most 30 s at 921600 baud, excluding host-side delays. | T |
| BNFR-003 | **Robustness:** 100 updates with random link interruptions and 20 with random power cuts shall all leave a bootable board. | T |
| BNFR-004 | **Rollback:** an image that never confirms shall be rejected and the previous image booted within 3 trial boots plus 3 × the IWDG timeout. | T |
| BNFR-005 | **Code quality:** `-Wall -Wextra -Werror` and cppcheck with no high-severity findings, as for HWVAL. | I |
| BNFR-006 | **Documentation:** a README shall describe the layout, provisioning, the boot flow and recovery over SWD. | I |

---

## 14. Host-side tooling requirements

| ID | Requirement | Method |
|---|---|---|
| BHOST-001 | A host tool `tools/mkimage.py` shall turn an application `.bin` plus version, name and commit into an image file (§5.1), and shall refuse binaries that violate IMG-002 or IMG-003. | T |
| BHOST-002 | `mkimage.py` shall have an `inspect` mode that prints and checks the header of an image file. | T |
| BHOST-003 | Provisioning (option bytes to dual-bank, mass erase, flashing BOOT over SWD) shall be done by the GUI ([GUI] §10). A script `tools/provision.sh` using OpenOCD in the Docker image shall do the same for CI. | D |
| BHOST-004 | A command-line updater `tools/bootctl.py` shall implement `info`, `update <image>`, `activate`, `reset` over HWLINK, sharing the protocol module with the GUI. | T |

---

## 15. Verification matrix

| Area | Requirements | Main method |
|---|---|---|
| Layout | LAY-001…005 | I, T |
| General | BGEN-001…010 | T |
| Image | IMG-001…007 | T |
| Selection and swap | SEL-001…005, SWP-001…003 | T |
| Trial and rollback | TRY-001…008 | T |
| Entry | ENT-001…005 | T, D |
| Protocol | UPD-001…010 | T |
| Hand-over | HND-001…005 | T |
| App contract | APP-001…007 | I, T (per app) |
| Safety | BSAFE-001…006 | I, T |
| Non-functional | BNFR-001…006 | T, I |
| Host tools | BHOST-001…004 | T, D |

---

## Appendix A — Boot flow

```
reset
  │
  ├─ init clocks, LEDs, USART3, CRC; LD2 on
  ├─ check nDBANK = 0 ─────────────── no ──> WRONG_FLASH_MODE, UART only, no flash writes
  ├─ read RTC_BKP0R (trial), RTC_BKP1R (stay magic, then clear)
  ├─ scan META A and META B, CRC-check slots
  ├─ pending slot with trials >= 3? ── yes ─> write REJECTED, rescan
  ├─ select valid slot with highest sequence number
  │
  ├─ stay magic, B1 held, or no valid slot? ── yes ─> UPDATE MODE
  ├─ listen 500 ms for Hello ──────────── got it ─> UPDATE MODE
  │
  ├─ pending? increment trial counter
  ├─ write hand-over record RTC_BKP2R
  ├─ start IWDG (4 s), de-init peripherals and clocks
  └─ copy swap+jump routine to RAM, set SWP_FB for slot B, jump to 0x0800C000

UPDATE MODE: serve HWLINK until Reset / Activate / 120 s idle → reset or boot
```

## Appendix B — Metadata record format

The metadata sector of each bank is written append-only after one erase at
`UpdateBegin`. Each record occupies its own 32-byte aligned area so that no
flash word is ever programmed twice.

| Offset in sector | Record | Contents | Written by |
|---|---|---|---|
| `0x000` | `HEADER` | The 256-byte image header (§5.1) | BOOT at `UpdateBegin` |
| `0x100` | `VERIFIED` | magic `0x56455249`, CRC32 read back from flash, CRC of record | BOOT at `UpdateEnd` |
| `0x120` | `ACTIVATED` | magic `0x41435456`, sequence number, CRC of record | BOOT at `Activate` |
| `0x140` | `CONFIRMED` | magic `0x434F4E46`, CRC of record | The application (APP-005) |
| `0x160` | `REJECTED` | magic `0x52454A45`, reason code, CRC of record | BOOT (TRY-003) |
| `0x180`..`0x27F` | `TRIAL` | magic `0x5452494C`, attempt number 1..8, CRC of record | BOOT before every pending hand-over (P2-6) |

A record is present only when its magic and CRC are correct; an erased
(`0xFF…`) area means "not present". A torn record (power lost while
programming) fails its CRC and is treated as not present.

Each record is exactly 32 bytes, little-endian:

| Word | Contents |
|---|---|
| 0 | magic (see the table above) |
| 1 | value: CRC32 read back (VERIFIED), sequence number (ACTIVATED), rejection reason (REJECTED), 0 (CONFIRMED) |
| 2..6 | zero |
| 7 | CRC-32/ISO-HDLC over bytes 0..27 |

The `HEADER` at offset `0x000` is the 256-byte image header of §5.1; its
CRC32 is the last word of those 256 bytes (`header_crc32`).

## Appendix C — Open items

| ID | Item | Status |
|---|---|---|
| OPEN-B01 | Confirm against [RM0410] that `SYSCFG_MEMRMP.SWP_FB` also swaps the ITCM flash alias (`0x0020 0000`), and how sector numbers in `FLASH_CR.SNB` map to physical banks while swapped. SWP-002/SWP-003 assume the AXIM alias is used and BOOT runs unswapped. | Open |
| OPEN-B02 | Measure the 128 KB sector erase time on this board (datasheet max ≈ 2 s at ×32 parallelism) and set the host `UpdateData` timeout ([GUI] FW-005) from it. | Open bench item |
| OPEN-B03 | Trial counter in a backup register restarts on power loss (no VBAT battery). Accept, or move the counter into append-only metadata records. | Closed: BOOT appends one `TRIAL` record per pending attempt (P2-6); the RTC register remains a hand-over hint. |
| OPEN-B04 | Self-update of BOOT (using the reserved bank 2 sectors 0–1 and `nDBOOT`) is out of scope; BOOT is updated over SWD. | Deferred |
| OPEN-B05 | Confirm that OpenOCD's `stm32f2x options_write` can set `nDBANK`/`nDBOOT` on the F767; otherwise provisioning uses STM32CubeProgrammer CLI. Changing `nDBANK` requires a full erase. | Open |
| OPEN-B06 | Earlier design notes named the F4 `BFB2` bit; the F76x mechanism is `SWP_FB` (runtime, volatile) plus `nDBOOT`/`BOOT_ADDx` (option bytes). This SRS uses `SWP_FB` only, so no option byte is written to switch slots. | Closed in this draft |
| OPEN-B07 | Confirm the flatcc runtime + verifier size fits BNFR-001 together with the flash and UART code. | Open |
