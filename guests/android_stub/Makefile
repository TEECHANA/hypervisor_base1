CROSS   ?= aarch64-linux-gnu-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
CFLAGS   = -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -nostdlib -nostdinc \
           -mcpu=cortex-a57 -mabi=lp64 -fno-stack-protector
LGCC     = $(shell $(CC) -print-libgcc-file-name)

TARGET  = android.bin
ELF     = build/android.elf
OBJS    = build/start.o build/android_main.o

.PHONY: all clean

all: $(TARGET)
	@echo "  [Android stub] $(TARGET) ready: $(shell wc -c <$(TARGET)) bytes"

$(TARGET): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) android.ld | build
	$(CC) $(CFLAGS) -T android.ld -o $@ $(OBJS)

build/start.o: start.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/android_main.o: android_main.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build/ $(TARGET)
