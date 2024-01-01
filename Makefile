#
# Makefile - Top-level build, clean and flash for the A/B bootloader
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI (STM32F767ZI)
#
# The firmware is built with CMake + Ninja inside the stm32-dev container
# (BCON-001), the same way as scripts/build.sh. Flashing and debugging also
# run in the container by default; set NATIVE=1 to use the host's OpenOCD,
# GDB and picocom instead (scripts/install-tools.sh).
#
#   make                build the firmware (same as make build)
#   make build          configure (if needed) and build
#   make clean          remove the build directory
#   make rebuild        clean, then build
#   make flash          build, then flash boot.elf, verify and reset
#   make erase          erase the whole flash
#   make reset          reset the board
#   make debug          build, flash, then GDB halted at reset
#   make gdbserver      OpenOCD GDB server on :3333
#   make serial         serial console on the ST-LINK VCP (115200 8N1)
#   make lint           clang-tidy static analysis
#   make lint-strict    clang-tidy, failing on any finding
#   make probe          detect the ST-LINK and the target chip
#   make size           show the firmware size
#   make tools-check    byte-compile the host tools (runs as part of build)
#   make test           host C tests, Python tests and contract check
#   make package-hwlink publish the canonical HWLINK contract bundle
#   make image          build the stm32-dev Docker image
#   make help           show this help
#
# Variables (override on the command line, e.g. make BUILD_TYPE=Debug):
#   BUILD_TYPE   Release | Debug                       (Release, as build.sh)
#   NATIVE       1 = flash/debug/serial with host tools (0)
#

# Windows: run every recipe under Git's POSIX sh, also from cmd.exe or
# PowerShell with only Git\cmd on PATH. Otherwise native make falls back to
# cmd.exe, which cannot run the recipes (quotes, pipes, find, rm -rf), and
# `sh scripts/...` is not found. Git's usr\bin goes first on PATH so find,
# sort and friends are the POSIX tools, not the Windows ones of the same name.
ifeq ($(OS),Windows_NT)
ifneq ($(origin SHELL),command line)
GIT_USR_BIN := $(firstword \
  $(if $(wildcard C:/Program?Files/Git/usr/bin/sh.exe),C:/Program?Files/Git/usr/bin) \
  $(if $(wildcard $(subst \,/,$(LOCALAPPDATA))/Programs/Git/usr/bin/sh.exe),$(subst \,/,$(LOCALAPPDATA))/Programs/Git/usr/bin) \
  $(if $(wildcard C:/msys64/usr/bin/sh.exe),C:/msys64/usr/bin))
ifneq ($(GIT_USR_BIN),)
SHELL := $(subst ?, ,$(GIT_USR_BIN))/sh.exe
export PATH := $(subst /,\,$(subst ?, ,$(GIT_USR_BIN)));$(PATH)
endif
endif
endif

BUILD_TYPE ?= Release
NATIVE     ?= 0
# On Windows `python3` is the Microsoft Store alias stub, which exits 9009, so
# the host tools run as plain `python` there.
ifeq ($(OS),Windows_NT)
PYTHON     ?= python
else
PYTHON     ?= python3
endif

BUILD_DIR  := build
ELF        := $(BUILD_DIR)/boot.elf
DEV        := scripts/docker-dev.sh
OPENOCD    := openocd -f board/st_nucleo_f7.cfg

# Gow make on Windows runs a simple recipe line itself instead of through a
# shell, and the MSYS bash.exe it starts that way exits 1 without running the
# script at all - no output, no status.  Prefixing the command with `sh` (Git
# Bash's own sh) takes the path make needs, so the script runs and its exit
# status is kept.  Linux and macOS start the scripts directly, as before.
ifeq ($(OS),Windows_NT)
DEV        := sh $(DEV)
RUN_SH     := sh
else
RUN_SH     :=
endif

.DEFAULT_GOAL := help

.PHONY: all build configure clean rebuild lint lint-strict flash erase reset \
        debug gdbserver serial probe size image tools-check \
        test test-host test-python test-contract package-hwlink help

all: build

# Reconfigure when the build directory is new or the build type changed;
# otherwise let Ninja's own dependency tracking decide what to rebuild.
configure:
	@if ! grep -qx "CMAKE_BUILD_TYPE:STRING=$(BUILD_TYPE)" $(BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "Configuring $(BUILD_TYPE) build in $(BUILD_DIR)/"; \
		$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE); \
	fi

build: configure tools-check
	$(DEV) run cmake --build /work/$(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean
	@"$(MAKE)" --no-print-directory build

# clang-tidy needs the compile commands from a configured build tree.
lint: configure
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

lint-strict:
	$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DBOOT_LINT_STRICT=ON
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

size:
	@test -f $(ELF) || { echo "error: $(ELF) not found, run make build"; exit 1; }
	$(DEV) run arm-none-eabi-size /work/$(ELF)

# The host tools are plain Python; compiling them keeps them from rotting as
# part of every build without requiring any package to be installed.  The
# flash GUI that drives them lives in the bootloader-gui repository.
tools-check:
	$(PYTHON) -m py_compile tools/bootctl.py tools/mkimage.py \
		tools/check_contracts.py tools/package_hwlink.py

test: test-host test-python test-contract

# Windows brings neither a native C compiler nor a pseudo-terminal, so the two
# host-side suites need to say what is missing instead of failing inside make.
ifeq ($(OS),Windows_NT)

test-host:
	@command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || { \
		echo "test-host needs a native C compiler: install mingw-w64, or run this on Linux/macOS."; \
		exit 1; }
	$(RUN_SH) tests/run_host_tests.sh

test-python:
	@echo "skip test-python: tests/python drives a simulated BOOT on a pty, which Windows does not have"

else

test-host:
	tests/run_host_tests.sh

test-python:
	$(PYTHON) -m pytest -q tests/python

endif

test-contract:
	$(PYTHON) tools/check_contracts.py

package-hwlink:
	$(PYTHON) tools/package_hwlink.py

image:
	$(DEV) build

ifeq ($(NATIVE),1)

flash: build
	$(OPENOCD) -c "program $(ELF) verify reset exit"

erase:
	$(OPENOCD) -c "init; reset halt; stm32f2x mass_erase 0; exit"

reset:
	$(OPENOCD) -c "init; reset run; exit"

debug: build
	$(OPENOCD) -l $(BUILD_DIR)/openocd.log & ocd=$$!; \
	trap 'kill $$ocd 2>/dev/null' EXIT; sleep 1; \
	gdb-multiarch -q $(ELF) -ex "target extended-remote :3333" -ex "monitor reset halt" -ex load

gdbserver:
	$(OPENOCD)

serial:
	$(RUN_SH) scripts/serial.sh

probe:
	st-info --probe

else

flash: build
	$(DEV) flash $(ELF)

erase:
	$(DEV) erase

reset:
	$(DEV) reset

debug: build
	$(DEV) debug $(ELF) --load

gdbserver:
	$(DEV) gdbserver

serial:
	$(DEV) serial

probe:
	$(DEV) probe

endif

help:
	@sed -n "/^#   make/p; /^# Variables/,/^#$$/p" $(firstword $(MAKEFILE_LIST)) | sed "s/^# \{0,1\}//"
