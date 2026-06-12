#ifndef HYP_CONFIG_H
#define HYP_CONFIG_H

#define MAX_VMS            4
#define MAX_VCPU_PER_VM    4
#define MAX_PHYS_CPUS      4
#define MAX_MEM_REGIONS    8
#define MAX_DEV_PER_VM     16
#define MAX_IRQ_ROUTES     256
#define MAX_MMIO_REGIONS   32
#define MAX_SHMEM_REGIONS  16

/* Scheduler: static cyclic-executive major frame */
#define SCHED_MAJOR_FRAME_US   10000u   /* 10 ms */

/* Log levels: 0=off 1=error 2=warn 3=info 4=debug */
#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

/* Platform UART bases */
#define UART_BASE_QEMU  0x09000000ULL
#define UART_BASE_RPI4  0xFE201000ULL
#define UART_BASE_S32G  0x401C8000ULL

/* GIC bases (QEMU virt) */
#define GICD_BASE_QEMU  0x08000000ULL
#define GICR_BASE_QEMU  0x080A0000ULL

/* Hypervisor load address */
#define HYP_LOAD_ADDR   0x40000000ULL

/* Physical memory layout (QEMU 2 GB) */
#define LINUX_VM_PA     0x41000000ULL
#define RTOS_VM_PA      0x60000000ULL
#define ANDROID_VM_PA   0x70000000ULL
#define SHMEM_POOL_PA   0xA0000000ULL
#define SHMEM_POOL_SZ   0x20000000ULL  /* 512 MB shared pool */

#endif /* HYP_CONFIG_H */
