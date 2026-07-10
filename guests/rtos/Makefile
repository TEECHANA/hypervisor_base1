CROSS   ?= aarch64-linux-gnu-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
CFLAGS   = -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -nostdlib -nostdinc \
           -mcpu=cortex-a57 -mabi=lp64 -fno-stack-protector

# Rogue-DMA attack scenario: when on, the RTOS issues real out-of-bounds DMA
# requests (HVC_DMA_XFER) that drive the hypervisor's detect->enforce->quarantine
# path. Default on (demo); build with ROGUE_DMA=0 for a well-behaved guest.
ROGUE_DMA ?= 1
ifeq ($(ROGUE_DMA),1)
CFLAGS   += -DVSE_ROGUE_DMA
endif

LGCC     = $(shell $(CC) -print-libgcc-file-name)

TARGET  = rtos.bin
ELF     = build/rtos.elf
OBJS    = build/start.o build/rtos.o

.PHONY: all clean

all: $(TARGET)
	@echo "  [RTOS guest] $(TARGET) ready: $(shell wc -c <$(TARGET)) bytes"

$(TARGET): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) rtos.ld | build
	$(CC) $(CFLAGS) -T rtos.ld -o $@ $(OBJS) $(LGCC)

build/start.o: start.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/rtos.o: rtos.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build/ $(TARGET)
