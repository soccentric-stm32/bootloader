#
# Host unit tests (x86) for the hardware-independent boot logic.
#
#   make -C tests/host
#
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -Wno-int-to-pointer-cast -g -O1
ROOT    := ../..
# BSAFE-006: the same micro-ecc configuration as the firmware, minus the
# Cortex-M assembly.  test_vectors.c supplies the host-test public key.
DEFINES := -DBOOT_SIGNED_IMAGES=1 \
           -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
           -DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256k1=0 \
           -DuECC_SUPPORT_COMPRESSED_POINT=0 -DuECC_OPTIMIZATION_LEVEL=1
INCLUDES := -Ifake_hal -I. -I$(ROOT)/src/common -I$(ROOT)/src/app \
            -I$(ROOT)/src/bsp -I$(ROOT)/third_party/micro-ecc
SOURCES := test_main.c test_vectors.c fake_hal.c fake_flash.c fake_crc32.c \
           $(ROOT)/src/common/image.c \
           $(ROOT)/src/common/metadata.c \
           $(ROOT)/src/common/sha256.c \
           $(ROOT)/src/app/selection.c \
           $(ROOT)/src/app/update.c \
           $(ROOT)/third_party/micro-ecc/uECC.c

.PHONY: all run clean

all: host_tests

host_tests: $(SOURCES)
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -o $@ $(SOURCES)

run: host_tests
	./host_tests

clean:
	rm -f host_tests
