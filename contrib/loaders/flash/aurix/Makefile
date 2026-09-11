# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the flash write loader.
#
# Requires a bare-metal TriCore gcc, e.g.:
#   make CROSS_COMPILE=/opt/tricore-gcc940/bin/tricore-

CROSS_COMPILE ?= tricore-
CC = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy

CFLAGS = -O3 -ffreestanding -nostdlib -fomit-frame-pointer -Wall -fno-unroll-loops \
	-nostartfiles -Wl,-T,tricore.ld

all: tc2x-pflash-program.inc tc2x-dflash-program.inc  \
	tc3x-pflash-program.inc tc3x-dflash-program.inc  \
	tc4x-pflash-program.inc tc4x-dflash-program.inc \
	tc4x-pflashcs-program.inc tc4x-dflashcs-program.inc \
	tc4x-rram-pnvm-program.inc tc4x-rram-dnvm-program.inc

define FLASH_DEFINE
-D$(shell echo $(notdir $@) | sed -e 's/-program\.inc$$//' -e 's/-/_FLASH_/' | tr '[:lower:]' '[:upper:]')
endef

define RRAM_DEFINE
-D$(shell echo $(notdir $@) | sed -e 's/-program\.inc$$//' -e 's/-/_/g' | tr '[:lower:]' '[:upper:]')
endef

tc2x-%-program.inc: aurix-flash-program.c
	$(CC) -mcpu=tc29xx $(CFLAGS) $(FLASH_DEFINE) -o $(basename $@).o $<
	$(OBJCOPY) -O binary -j .text $(basename $@).o $(basename $@).bin
	../../../../src/helper/bin2char.sh < $(basename $@).bin > $@

tc3x-%-program.inc: aurix-flash-program.c
	$(CC) -mcpu=tc39xx $(CFLAGS) $(FLASH_DEFINE) -o $(basename $@).o $<
	$(OBJCOPY) -O binary -j .text $(basename $@).o $(basename $@).bin
	../../../../src/helper/bin2char.sh < $(basename $@).bin > $@

tc4x-%-program.inc: aurix-flash-program.c
	$(CC) -mcpu=tc4DAx $(CFLAGS) $(FLASH_DEFINE) -o $(basename $@).o $<
	$(OBJCOPY) -O binary -j .text $(basename $@).o $(basename $@).bin
	../../../../src/helper/bin2char.sh < $(basename $@).bin > $@

tc4x-rram-%-program.inc: aurix-rram-program.c
	$(CC) -mcpu=tc4DAx $(CFLAGS) $(RRAM_DEFINE) -o $(basename $@).o $<
	$(OBJCOPY) -O binary -j .text $(basename $@).o $(basename $@).bin
	../../../../src/helper/bin2char.sh < $(basename $@).bin > $@

clean:
	rm -f *.o *.bin *.inc