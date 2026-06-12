# Tessolve Hypervisor with Guest OSes

Type-1 ARM64 bare-metal hypervisor running **Linux**, **RTOS** and **Android**
concurrently. Targets QEMU virt / Raspberry Pi 4 / NXP S32G.

## Quick Start

### Option A — One-shot script
```bash
apt install gcc-aarch64-linux-gnu qemu-system-arm make
bash build_and_run.sh
```

### Option B — Docker (hermetic, no host deps)
```bash
docker build -t tessolve-hyp .
docker run --rm -it tessolve-hyp
```

### Option C — Manual steps
```bash
# 1. Install tools
apt install gcc-aarch64-linux-gnu qemu-system-arm make

# 2. Build everything (hypervisor + all 3 guest OSes)
make all PLATFORM=qemu

# 3. Run in QEMU with all guests loaded
make run-with-guests

# 4. Debug with GDB
make debug
# In another terminal:
aarch64-linux-gnu-gdb build/qemu/hypervisor.elf \
    -ex "target remote :1234" \
    -ex "b hyp_main"
```

## What you will see in the QEMU serial output

```
HYP [INF] ==================================
HYP [INF]   Tessolve Hypervisor v1.0  booting
HYP [INF]   Platform : QEMU-ARM64-virt
HYP [INF] ==================================
HYP [INF] Init GICv3...
HYP [INF] Init VM subsystem...
HYP [INF] VM 1 'linux' created
HYP [INF] VM 2 'rtos'  created
HYP [INF] VM 3 'android' created
HYP [INF] DTB built: 512 bytes at PA 0x43000000
HYP [INF] Scheduler ready — starting scheduler
HYP [INF] ERET -> VM 1 'linux' vCPU 0 entry=0x41200000

==============================================
  Tessolve Hypervisor — Linux Guest Demo
  EL1h, MMU off, UART @ 0x09000000
==============================================
  DTB IPA  : 0x0000000002000000
  VM ID    : 0x0000000000000001
  IPC send -> RTOS: rc=0x0000000000000000
  Entering idle loop...

[RTOS] Booted. VM ID=2
[RTOS] IPC rx: ping-from-linux
[RTOS] heartbeat #100 batch

[ANDROID] Guest boot stub active
```

## Directory layout

```
arch/arm64/          EL2 entry, vectors, context-switch, MMU bootstrap
boot/
  main.c             Primary C entry
  platform_init.c    VM creation: Linux + RTOS + Android
  dtb/dtb_gen.c      Minimal FDT/DTB builder for Linux guest
  pal/               Platform abstraction (QEMU / RPi4 / S32G)
  linker/            ELF linker script
core/
  vm/                VM descriptor lifecycle
  vcpu/              vCPU state + EL1 system registers
  sched/             Static cyclic-executive scheduler
  fault/             DABT/IABT fault handler
vre/
  mmu/stage2.c       Stage-2 IPA→PA page tables (4 KB, 3-level)
  irq/               Physical→virtual IRQ routing
  device/            MMIO trap & emulation
  dma/               ARM SMMUv3 stream-table
guest/
  hypercall/         HVC #0 dispatch (VM mgmt, shmem, IPC, log)
  shmem/             Inter-VM shared memory (dual Stage-2 mapping)
  ipc/               Inter-VM message queues
drivers/
  uart/              PL011 UART
  gic/               GICv3 distributor + virtual IRQ injection
  timer/             ARM Generic Timer (CNTHP)
lib/
  log/               Structured logging
  str/               string/memory ops (no libc)
guests/
  linux/             Linux demo guest + real kernel build support
  rtos/              Bare-metal RTOS (cooperative scheduler, IPC)
  android_stub/      Android boot stub (replace Image for production)
configs/             Per-VM YAML configuration
tests/               Unit + integration tests
Dockerfile           Hermetic build container
build_and_run.sh     One-shot build+run
```

## Physical Memory Map (QEMU 2 GB)

| Region             | Physical Address    | Size   |
|--------------------|---------------------|--------|
| Hypervisor core    | 0x40000000          | 16 MB  |
| Linux VM           | 0x41000000          | 496 MB |
| Linux DTB          | 0x43000000          | 4 KB   |
| RTOS VM            | 0x60000000          | 256 MB |
| Android VM         | 0x70000000          | 512 MB |
| Shared memory pool | 0xA0000000          | 512 MB |

## Hypercall ABI (HVC #0)

| ID     | Name           | Args                           | Return       |
|--------|----------------|--------------------------------|--------------|
| 0x0001 | VM_GET_ID      | —                              | vm_id        |
| 0x0002 | VM_QUERY_STATE | x1=vm_id                       | vm_state_t   |
| 0x0003 | VM_STOP        | x1=vm_id                       | err          |
| 0x0010 | SHMEM_MAP      | x1=src x2=dst x3=ipa x4=size   | err          |
| 0x0011 | SHMEM_UNMAP    | x1=ipa x2=size                 | err          |
| 0x0020 | IPC_SEND       | x1=dst_vm x2=buf_ipa x3=len    | err          |
| 0x0021 | IPC_RECV       | x1=buf_ipa x2=max_len          | len or err   |
| 0x0022 | IPC_NOTIFY     | x1=dst_vm                      | err          |
| 0x00F0 | LOG_WRITE      | x1=buf_ipa x2=len              | err          |

## Using a Real Linux Kernel

```bash
# Build Linux 6.6 LTS for the guest (takes ~10 min)
make -C guests/linux kernel

# Then run with the real kernel
make run-with-guests
```

## Using a Real Android Kernel (GKI)

Download Android Generic Kernel Image for arm64:
```bash
# From AOSP: https://source.android.com/docs/core/architecture/kernel/download
cp /path/to/android/Image guests/android_stub/Image
```

## Scheduler Configuration

The hypervisor uses a **static cyclic-executive** scheduler.
Edit `configs/` YAML files to change time slices:

| VM      | Default slice | Priority | Realtime |
|---------|--------------|----------|----------|
| Linux   | 5 ms         | 2        | No       |
| RTOS    | 1 ms         | 0        | Yes      |
| Android | 4 ms         | 2        | No       |
