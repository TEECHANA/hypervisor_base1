#ifndef DTB_H
#define DTB_H

#include "../../include/types.h"
#include "../../include/error.h"

/*
 * dtb_build_linux — build a minimal FDT for the Linux guest.
 *
 * buf              : output buffer
 * buf_sz           : size of buf
 * ram_base         : IPA of the first RAM region base
 * ram_size         : size of the first RAM region
 * uart_base        : IPA of the PL011 UART
 * uart_irq         : GIC SPI number (absolute, e.g. 33)
 * initrd_start_ipa : IPA where initramfs starts in guest address space
 * initrd_end_ipa   : IPA where initramfs ends  in guest address space
 * out_sz           : bytes written to buf
 *
 * IPA derivation example (default QEMU layout):
 *   QEMU loads initramfs at PA 0x47000000.
 *   Linux RAM region 1 maps IPA 0x0 → PA 0x41000000.
 *   initrd_start_ipa = 0x47000000 - 0x41000000 = 0x06000000
 *   initrd_end_ipa   = 0x06800000  (8 MB, safe upper bound)
 */
err_t dtb_build_linux(void *buf, u64 buf_sz,
                      u64 ram_base, u64 ram_size,
                      u64 uart_base, u32 uart_irq,
                      u64 initrd_start_ipa, u64 initrd_end_ipa,
                      u64 *out_sz);

#endif /* DTB_H */
