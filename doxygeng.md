# Doxygen Documentation Progress

Task: add maximum-detail Doxygen documentation (`/** ... */`) to every
project-owned `.c` / `.h` file. One file per commit, each commit pushed.

**Excluded (out of scope, intentional):**
- `third_party/` — vendored upstream code (flatcc 0.6, micro-ecc); do not modify vendor trees.
- `build/` — CMake build artifacts (`CMakeCCompilerId.c`).
- `src/protocol/generated/` — flatcc auto-generated builder/reader/verifier;
  regenerating would wipe any hand-edits.

## File checklist

### board/
- [x] `board/boot_board.h`
- [x] `board/stm32f7xx_hal_conf.h`

### keys/
- [x] `keys/dev_pubkey.c`

### src/app/
- [x] `src/app/handover.c`
- [x] `src/app/handover.h`
- [x] `src/app/main.c`
- [x] `src/app/selection.c`
- [x] `src/app/selection.h`
- [x] `src/app/update.c`
- [x] `src/app/update.h`

### src/bsp/
- [x] `src/bsp/clock.c`
- [x] `src/bsp/clock.h`
- [ ] `src/bsp/fault.c`
- [ ] `src/bsp/flash.c`
- [ ] `src/bsp/flash.h`
- [ ] `src/bsp/iwdg.c`
- [ ] `src/bsp/iwdg.h`
- [ ] `src/bsp/uart3.c`
- [ ] `src/bsp/uart3.h`

### src/common/
- [ ] `src/common/boot_layout.h`
- [ ] `src/common/crc32.c`
- [ ] `src/common/crc32.h`
- [ ] `src/common/image.c`
- [ ] `src/common/image.h`
- [ ] `src/common/metadata.c`
- [ ] `src/common/metadata.h`
- [ ] `src/common/sha256.c`
- [ ] `src/common/sha256.h`
- [ ] `src/common/signing_key.h`

### src/protocol/
- [ ] `src/protocol/flatcc_port.c`
- [ ] `src/protocol/flatcc_port.h`
- [ ] `src/protocol/hwlink.c`
- [ ] `src/protocol/hwlink.h`
- [ ] `src/protocol/proto.c`
- [ ] `src/protocol/proto.h`

### tests/host/
- [ ] `tests/host/fake_crc32.c`
- [ ] `tests/host/fake_flash.c`
- [ ] `tests/host/fake_flash.h`
- [ ] `tests/host/fake_hal.c`
- [ ] `tests/host/fake_hal/stm32f7xx_hal.h`
- [ ] `tests/host/test_main.c`
- [ ] `tests/host/test_vectors.c`
- [ ] `tests/host/test_vectors.h`

## Log
- started
- board/boot_board.h, board/stm32f7xx_hal_conf.h done
- keys/dev_pubkey.c done
- src/app/handover.h, src/app/handover.c done
