# Source layout

The firmware is split into four layers.  Dependency direction is one-way:
`app` may use any lower layer, `protocol` and `bsp` may use `common`, and
`common` depends on neither the HAL nor the protocol.

```
src/
├── app/                     Boot flow and application logic
│   ├── main.c               Entry point, BOOT state machine
│   ├── selection.c/.h       A/B slot selection, trial-boot/rollback state
│   ├── update.c/.h          Update transaction (erase, program, verify)
│   ├── handover.c/.h        Clock/IRQ teardown and jump to the application
│   └── swap_jump.S          Bank swap + jump, must run from DTCM
│
├── bsp/                     Board support package (STM32F7 + NUCLEO-F767ZI)
│   ├── clock.c/.h           HSE/PLL/over-drive bring-up and teardown
│   ├── flash.c/.h           Bank/sector erase, program, CRC read-back
│   ├── uart3.c/.h           USART3 + DMA receive ring (ST-LINK VCP)
│   ├── iwdg.c/.h            Independent watchdog
│   └── fault.c              HardFault handler (BGEN-006)
│
├── common/                  Hardware-independent helpers
│   ├── boot_layout.h        Flash map constants (normative, see Jenkinsfile)
│   ├── crc32.c/.h           CRC32 of image and metadata records
│   ├── image.c/.h           Image header parse/validate, signature check
│   ├── metadata.c/.h        Append-only metadata record log
│   ├── sha256.c/.h          SHA-256 for image authentication (BSAFE-006)
│   └── signing_key.h        Built-in public key (defined in keys/*.c)
│
└── protocol/                HWLINK wire protocol
    ├── hwlink.c/.h          Frame codec (COBS + CRC32)
    ├── proto.c/.h           Message dispatch / server loop
    ├── flatcc_port.c/.h     Fixed-arena flatcc allocator hooks (no malloc)
    └── generated/           FlatBuffers C headers from protocol/hwlink.fbs
```

`board/` (HAL configuration and board pin map) and `ld/` (linker script)
stay at the repository root, as do `protocol/hwlink.fbs` and `third_party/`.

## Adding a module

1. Put the code in the layer it belongs to (see the dependency direction
   above).
2. Add the `.c` file to the matching `*_SOURCES` list in `CMakeLists.txt`
   (`APP_SOURCES`, `BSP_SOURCES`, `COMMON_SOURCES`, `PROTOCOL_SOURCES`).
3. Headers are found by name (`#include "clock.h"`); each layer directory is
   on the include path, so no relative paths are needed.
4. Keep `boot_layout.h` in `common/` — the Jenkins pipeline and the host
   tools parse it for the flash map.
