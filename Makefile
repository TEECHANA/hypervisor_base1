# ═══════════════════════════════════════════════════════════════════
#  Tessolve Hypervisor — Master Makefile
# ═══════════════════════════════════════════════════════════════════

PLATFORM    ?= qemu
BUILD_DIR   = build/$(PLATFORM)
HYP_ELF     = $(BUILD_DIR)/hypervisor.elf

CROSS       ?= aarch64-linux-gnu-
CC          = $(CROSS)gcc
OBJCOPY     = $(CROSS)objcopy
LGCC        = $(shell $(CC) -print-libgcc-file-name)
INITRD_SIZE := $(shell stat -c%s guests/linux/initramfs.cpio.gz 2>/dev/null || echo 0)
CFLAGS  = -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffreestanding -nostdlib -nostdinc
CFLAGS += -mcpu=cortex-a57 -mabi=lp64
CFLAGS += -DPLATFORM_$(shell echo $(PLATFORM) | tr a-z A-Z)
CFLAGS += -I.
CFLAGS += -DVSE_IDS_DEMO   # IDS attack demo on boot; build with VSE_IDS_DEMO=0 to disable
CFLAGS += -DINITRD_SIZE=$(INITRD_SIZE)  # <--- ADD THIS LINE
ASM_SRCS = arch/arm64/entry.S arch/arm64/context.S arch/arm64/mmu.S

C_SRCS = \
    boot/main.c                   \
    boot/platform_init.c          \
    boot/pal/pal_$(PLATFORM).c    \
    boot/dtb/dtb_gen.c            \
    arch/arm64/psci.c             \
    core/vm/vm.c                  \
    core/vcpu/vcpu.c \
	core/vcpu/pmu.c              \
    core/sched/sched.c \
	core/sched/sched_stats.c \
	core/sched/sched_stats.c \
	core/sched/sched_stats.c \
	core/sched/sched_stats.c            \
    core/fault/fault.c            \
    vre/mmu/stage2.c              \
    vre/irq/irq_router.c \
	vre/irq/vgic.c          \
	vre/bus/pcie.c           \
    vre/device/mmio_trap.c        \
    vre/dma/smmu.c \
	vre/dma/dma_guard.c \
	vre/power/power.c \
	boot/device_profile.c                \
    guest/hypercall/hvc_handler.c \
	guest/hypercall/hvc_drivers.c   \
    guest/shmem/shmem.c           \
    guest/ipc/ipc.c               \
    drivers/uart/uart_pl011.c     \
    drivers/gic/gicv3.c           \
    drivers/timer/timer.c         \
    lib/log/log.c                 \
    lib/str/string.c		  \
    arch/arm64/s2_debug.c         \
    vse/hmac.c                    \
    vse/config_check.c            \
    vse/component_check.c         \
    vse/trust.c                   \
    vse/seal.c                    \
    vse/fault_detect.c            \
    vse/failover.c                \
    vse/hotp.c                    \
    vse/login.c                   \
    vse/ids.c                     \
    vse/guest_measure.c

HYP_OBJS = $(ASM_SRCS:%.S=$(BUILD_DIR)/%.o) \
           $(C_SRCS:%.c=$(BUILD_DIR)/%.o)

LINUX_IMG   = guests/linux/Image
RTOS_IMG    = guests/rtos/rtos.bin
ANDROID_IMG = guests/android_stub/android.bin

QEMU_BASE = qemu-system-aarch64 \
    -machine virt,gic-version=3,virtualization=on,iommu=smmuv3\
    -cpu cortex-a57 \
    -m 2G -smp 4 \
    -nographic \
    -serial mon:stdio \
    -no-reboot \
    -d guest_errors
    #-d guest_errors,unimp

.PHONY: all clean run run-with-guests debug
.PHONY: guests guest-linux guest-rtos guest-android
.PHONY: qemu rpi4 s32g test-unit test-integration

# ────────────────────────────────────────────────────────────────
#all: $(HYP_ELF) guests
all: $(HYP_ELF)
# ── Hypervisor Build ────────────────────────────────────────────
$(HYP_ELF): $(HYP_OBJS)
	@mkdir -p $(@D)
	$(CC) -T boot/linker/hypervisor.ld -Wl,--build-id=none -nostdlib \
	    -o $@ $^ $(LGCC)
	@$(CROSS)size $@
	@echo ""
	@echo "  ✓ Hypervisor built: $@"

# ── Compilation Rules ───────────────────────────────────────────
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  AS  $<"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  CC  $<"

# ── Guest OS Builds ─────────────────────────────────────────────
guests: guest-linux guest-rtos guest-android

guest-linux:
	@echo "--- Building Linux guest ---"
	$(MAKE) -C guests/linux CROSS=$(CROSS) demo

guest-rtos:
	@echo "--- Building RTOS guest ---"
	$(MAKE) -C guests/rtos CROSS=$(CROSS)

guest-android:
	@echo "--- Building Android stub guest ---"
	$(MAKE) -C guests/android_stub CROSS=$(CROSS)

# ── Validate Guest Images ───────────────────────────────────────
check-guests:
	@test -f $(LINUX_IMG)   || (echo "ERROR: Missing $(LINUX_IMG)"; exit 1)
	@test -f $(RTOS_IMG)    || (echo "ERROR: Missing $(RTOS_IMG)"; exit 1)
	@test -f $(ANDROID_IMG) || (echo "ERROR: Missing $(ANDROID_IMG)"; exit 1)
	@echo "  ✓ All guest images present"

# ── Run Hypervisor Only ─────────────────────────────────────────
qemu-run: $(HYP_ELF)
	$(QEMU_BASE) -kernel $(HYP_ELF)

# ── Run with Guests ─────────────────────────────────────────────
run-with-guests: $(HYP_ELF) check-guests
	@echo "=== Launching QEMU with guests ==="
	$(QEMU_BASE) \
	    -kernel $(HYP_ELF) \
	    -device loader,file=$(LINUX_IMG),addr=0x41200000,force-raw=on \
	    -device loader,file=guests/linux/initramfs.cpio.gz,addr=0x47000000,force-raw=on \
	    -device loader,file=$(RTOS_IMG),addr=0x60008000,force-raw=on \
	    -device loader,file=$(ANDROID_IMG),addr=0x70200000,force-raw=on

# ── Debug Mode ─────────────────────────────────────────────────
debug: $(HYP_ELF) check-guests
	@echo "GDB: target remote :1234"
	$(QEMU_BASE) \
	    -kernel $(HYP_ELF) \
	    -device loader,file=$(LINUX_IMG),addr=0x41000000,force-raw=on \
	    -device loader,file=guests/linux/initramfs.cpio.gz,addr=0x47000000,force-raw=on \
	    -device loader,file=$(RTOS_IMG),addr=0x60008000,force-raw=on \
	    -device loader,file=$(ANDROID_IMG),addr=0x70200000,force-raw=on \
	    -s -S

# ── Platform Shortcuts ─────────────────────────────────────────
#qemu:
#	$(MAKE) PLATFORM=qemu CROSS=$(CROSS)
qemu: $(HYP_ELF)
rpi4:
	$(MAKE) PLATFORM=rpi4 CROSS=aarch64-linux-gnu-

s32g:
	$(MAKE) PLATFORM=s32g CROSS=aarch64-linux-gnu-

# ── Tests ─────────────────────────────────────────────────────
test-unit:
	bash tests/unit/run_tests.sh

test-integration:
	bash tests/integration/run_qemu_tests.sh

# ── Clean ─────────────────────────────────────────────────────
clean:
	rm -rf build/
	@if [ -f guests/linux/Makefile ]; then \
		$(MAKE) -C guests/linux clean || true; \
	fi
