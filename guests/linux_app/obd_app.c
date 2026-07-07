#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

/* The hypervisor exports OBD data at a fixed IPA: 0x4F000000
 * Linux maps this via /dev/mem to read it directly.
 * Layout (64 bytes):
 *   0x00: magic (0x4F424400 = "OBD\0")
 *   0x04: rpm
 *   0x08: speed_kmh
 *   0x0C: coolant_c
 *   0x10: throttle_pct
 *   0x14: map_kpa
 *   0x18: dtc_count
 *   0x1C: dtc_code
 *   0x20: seq
 *   0x24: cyl_status (bits 0-3)
 *   0x28: inj_count
 */
#define OBD_SHMEM_IPA   0x0A000000UL
#define OBD_MAGIC       0x4F424400U

typedef struct {
    uint32_t magic;
    uint32_t rpm;
    uint32_t speed_kmh;
    uint32_t coolant_c;
    uint32_t throttle_pct;
    uint32_t map_kpa;
    uint32_t dtc_count;
    uint32_t dtc_code;
    uint32_t seq;
    uint32_t cyl_status;
    uint32_t inj_count;
} __attribute__((packed)) obd_shmem_t;

int main(void)
{
    printf("\n==================================\n");
    printf("INIT SCRIPT STARTED\n");
    printf("==================================\n\n");
    printf("==================================================\n");
    printf("  Tessolve Hypervisor -- Linux guest userspace up\n");
    printf("==================================================\n\n");

    printf("[OBD] *** Tessolve OBD-II Diagnostic Monitor v1.0 ***\n");
    printf("[OBD] Protocol: shared memory IPA=0x%lX\n", OBD_SHMEM_IPA);
    printf("[OBD] Data source: RTOS Fuel ECU (VM2) via hypervisor\n\n");
    fflush(stdout);

    /* Open /dev/mem to access hypervisor shared memory region */
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        printf("[OBD] /dev/mem not available — showing waiting state\n");
        fflush(stdout);
        /* Fallback: just loop printing "waiting" */
        unsigned long slice = 1;
        while (1) {
            printf("[OBD] ---- Linux slice %lu ----\n", slice++);
            printf("[OBD] Waiting for RTOS ECU data (no /dev/mem)\n\n");
            fflush(stdout);
            sleep(2);
        }
        return 0;
    }

    volatile obd_shmem_t *obd = (volatile obd_shmem_t *)mmap(
        NULL, 4096, PROT_READ, MAP_SHARED, fd, OBD_SHMEM_IPA);

    if (obd == MAP_FAILED) {
        printf("[OBD] mmap failed — /dev/mem accessible but IPA not mapped\n");
        fflush(stdout);
        close(fd);
        unsigned long slice = 1;
        while (1) {
            printf("[OBD] ---- Linux slice %lu ----\n", slice++);
            printf("[OBD] Waiting for hypervisor OBD shmem mapping\n\n");
            fflush(stdout);
            sleep(2);
        }
        return 0;
    }

    unsigned long slice = 1;
    uint32_t last_seq = 0;

    while (1) {
        printf("[OBD] ---- Linux slice %lu ----\n", slice++);

        if (obd->magic != OBD_MAGIC) {
            printf("[OBD] No ECU data yet (magic=0x%08X)\n\n", obd->magic);
        } else {
            uint32_t seq = obd->seq;
            printf("[OBD] PID 0x0C RPM          : %u\n",     obd->rpm);
            printf("[OBD] PID 0x0D Speed        : %u km/h\n", obd->speed_kmh);
            printf("[OBD] PID 0x05 Coolant      : %u C\n",    obd->coolant_c);
            printf("[OBD] PID 0x11 Throttle     : %u %%\n",   obd->throttle_pct);
            printf("[OBD] PID 0x0B MAP          : %u kPa\n",  obd->map_kpa);
            uint32_t cs = obd->cyl_status;
            printf("[OBD] Cylinders: %s %s %s %s\n",
                   (cs&1)?"Cyl1:OK":"Cyl1:MISS",
                   (cs&2)?"Cyl2:OK":"Cyl2:MISS",
                   (cs&4)?"Cyl3:OK":"Cyl3:MISS",
                   (cs&8)?"Cyl4:OK":"Cyl4:MISS");
            if (obd->dtc_count)
                printf("[OBD] ** DTC: P0%03X (%u faults) **\n",
                       obd->dtc_code, obd->dtc_count);
            else
                printf("[OBD] DTC: none\n");
            printf("[OBD] seq=%u inj#=%u %s\n\n",
                   seq, obd->inj_count,
                   seq != last_seq ? "(updated)" : "(stale)");
            last_seq = seq;
        }
        fflush(stdout);
        sleep(2);
    }
    return 0;
}
