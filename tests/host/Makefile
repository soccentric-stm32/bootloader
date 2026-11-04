#
# Host unit tests (x86) for the hardware-independent boot logic.
#
#   make -C tests/host
#
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -Wno-int-to-pointer-cast -g -O1
ROOT    := ../..
INCLUDES := -Ifake_hal -I$(ROOT)/src/common -I$(ROOT)/src/app \
            -I$(ROOT)/src/bsp
SOURCES := test_main.c fake_hal.c fake_flash.c fake_crc32.c \
           $(ROOT)/src/common/image.c \
           $(ROOT)/src/common/metadata.c \
           $(ROOT)/src/app/selection.c \
           $(ROOT)/src/app/update.c

.PHONY: all run clean

all: host_tests

host_tests: $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SOURCES)

run: host_tests
	./host_tests

clean:
	rm -f host_tests
