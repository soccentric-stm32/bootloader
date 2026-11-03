#
# CMakeLists.txt - A/B bootloader (BOOT), NUCLEO-F767ZI
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  STM32F767ZIT6
# SRS:     SRS-BOOT-F767 (BCON-001..003, BCON-005, BGEN-001, LAY-001, BNFR-001)
#
# Build (inside the stm32-dev container, BCON-001):
#   cmake -S . -B build -G Ninja
#   cmake --build build
# or simply: scripts/build.sh [clean]
#
# Outputs in build/: boot.elf, boot.bin, boot.hex, boot.map (BGEN-001).
#

cmake_minimum_required(VERSION 3.22)

set(BOOT_VERSION "0.1.0")

# Cross toolchain: the build runs in the container where arm-none-eabi-gcc
# is the only compiler. Set explicitly so a stray host compiler is never used.
set(CMAKE_SYSTEM_NAME     Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY) # no OS to run a test exe in
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # compile_commands.json for clang-tidy (make lint)
find_program(ARMCC arm-none-eabi-gcc REQUIRED)
find_program(ARMOBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(SIZE_TOOL arm-none-eabi-size REQUIRED)
set(CMAKE_C_COMPILER   ${ARMCC})
set(CMAKE_ASM_COMPILER ${ARMCC})
set(CMAKE_OBJCOPY      ${ARMOBJCOPY})

project(boot LANGUAGES C ASM)

# --- SDK location ----------------------------------------------------------
# STM32CubeF7 v1.17.4 is provided by the docker image (BCON-002).
set(CUBE_ROOT "$ENV{STM32CUBE_F7}")
if(NOT CUBE_ROOT)
    set(CUBE_ROOT "/opt/st/STM32CubeF7")
endif()
if(NOT EXISTS "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_hal.h")
    message(FATAL_ERROR
        "STM32CubeF7 not found at ${CUBE_ROOT}.\n"
        "Build through scripts/docker-dev.sh (see docker/README.md).")
endif()

# --- Build metadata (BGEN-002) ---------------------------------------------
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE BOOT_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TIMESTAMP BOOT_BUILD_DATE "%Y-%m-%d %H:%M:%S")

# --- Toolchain flags --------------------------------------------------------
set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Size-optimised in every build type: BOOT must fit in 32 KB (BCON-005).
add_compile_options(${CPU_FLAGS} -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
# BCON-003: no dynamic memory and no stdio, so no syscall layer is linked.
add_link_options(${CPU_FLAGS} -Wl,--gc-sections -Wl,--cref
    -Wl,-Map=${CMAKE_BINARY_DIR}/boot.map
    --specs=nano.specs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-Os -g3")
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3")

add_compile_definitions(
    STM32F767xx
    USE_HAL_DRIVER
    HSE_VALUE=8000000U          # ST-LINK MCO feeds PH0 in bypass mode
    BOOT_VERSION_STR="${BOOT_VERSION}"
    BOOT_SRS_ID="SRS-BOOT-F767"
    BOOT_GIT_COMMIT="${BOOT_GIT_COMMIT}"
    BOOT_BUILD_DATE="${BOOT_BUILD_DATE}")

include_directories(src board
    "${CUBE_ROOT}/Drivers/CMSIS/Include"
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Inc")

# --- Sources ----------------------------------------------------------------
# HAL modules used so far. The list grows with each SRS section.
set(HAL_SOURCES
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_cortex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_rcc.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_rcc_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_gpio.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_pwr.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_pwr_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_flash.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_flash_ex.c")

set(PROJECT_SOURCES
    src/main.c
    src/handover.c
    src/fault.c)

set(DEVICE_SOURCES
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/system_stm32f7xx.c"
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/gcc/startup_stm32f767xx.s")

add_executable(boot.elf ${PROJECT_SOURCES} ${HAL_SOURCES} ${DEVICE_SOURCES})

# LAY-001: BOOT lives in bank 1 sectors 0-1 (32 KB); the linker script
# fails the build if the image grows past 0x08007FFF.
target_link_options(boot.elf PRIVATE
    "-T${CMAKE_SOURCE_DIR}/ld/boot.ld")

# --- Static analysis (BNFR-005) ----------------------------------------------
# clang-tidy reuses the compile flags this very configure step produced, so
# it must run from a configured build tree:  make lint / make lint-strict.
find_program(CLANG_TIDY clang-tidy REQUIRED)
option(BOOT_LINT_STRICT "Treat any clang-tidy finding as an error (lint-strict)" OFF)
if(BOOT_LINT_STRICT)
    set(BOOT_LINT_FLAGS --warnings-as-errors=*)
endif()

# The target runs with the build dir as CWD, so make source paths absolute.
set(BOOT_LINT_SOURCES)
foreach(_src ${PROJECT_SOURCES})
    list(APPEND BOOT_LINT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
endforeach()
add_custom_target(clang-tidy
    COMMAND ${CLANG_TIDY} -quiet -p ${CMAKE_BINARY_DIR} ${BOOT_LINT_FLAGS}
            ${BOOT_LINT_SOURCES}
    COMMENT "Running clang-tidy over the project sources"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)

set(ELF "$<TARGET_FILE:boot.elf>")
add_custom_command(TARGET boot.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary  ${ELF} ${CMAKE_BINARY_DIR}/boot.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex    ${ELF} ${CMAKE_BINARY_DIR}/boot.hex
    COMMAND ${SIZE_TOOL} $<TARGET_FILE:boot.elf>
    BYPRODUCTS ${CMAKE_BINARY_DIR}/boot.bin ${CMAKE_BINARY_DIR}/boot.hex
    COMMENT "Generating boot.bin and boot.hex")
