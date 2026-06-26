/* ARM PL011 UART — QEMU virt / Raspberry Pi 4 */
#include "uart.h"
#define DR   0x000u
#define FR   0x018u
#define IBRD 0x024u
#define FBRD 0x028u
#define LCR  0x02Cu
#define CR   0x030u
#define FR_TXFF (1u<<5)
#define FR_RXFE (1u<<4)
#define CR_UARTEN (1u<<0)
#define CR_TXE  (1u<<8)
#define CR_RXE  (1u<<9)
#define LCR_FEN (1u<<4)
#define LCR_8N1 (3u<<5)

static volatile u32 *_b;
static void wr(u32 o,u32 v){*(volatile u32*)((u8*)_b+o)=v;}
static u32  rd(u32 o){return *(volatile u32*)((u8*)_b+o);}

err_t uart_init(u64 base){
    _b=(volatile u32*)(uintptr_t)base;
    wr(CR,0); wr(IBRD,26); wr(FBRD,3);
    wr(LCR,LCR_FEN|LCR_8N1);
    wr(CR,CR_UARTEN|CR_TXE|CR_RXE);
    return E_OK;
}
void uart_putc(char c){
    if(!_b)return;
    while(rd(FR)&FR_TXFF){}
    wr(DR,(u32)c);
    if(c=='\n'){while(rd(FR)&FR_TXFF){}wr(DR,'\r');}
}
void uart_puts(const char*s){while(*s)uart_putc(*s++);}
static const char hx[]="0123456789abcdef";
void uart_puthex(u64 v){
    uart_puts("0x");
    for(int i=60;i>=0;i-=4)uart_putc(hx[(v>>i)&0xF]);
}
void uart_putdec(s64 v){
    if(v<0){uart_putc('-');v=-v;}
    if(!v){uart_putc('0');return;}
    char b[20];int i=0;
    while(v>0){b[i++]='0'+(int)(v%10);v/=10;}
    while(--i>=0)uart_putc(b[i]);
}

/* ── Receive path (added for operator login) ── */
char uart_getc(void){
    if(!_b) return 0;
    while(rd(FR)&FR_RXFE){}            /* wait until RX FIFO not empty */
    return (char)(rd(DR)&0xFF);
}

/*
 * uart_getline — read a line into buf (max len-1 chars), NUL-terminated.
 * Echoes input. If echo_mask is non-zero, prints that char instead of the
 * typed character (for password entry). Backspace handled. Returns length.
 */
u32 uart_getline(char *buf, u32 len, char echo_mask){
    u32 i=0;
    if(!buf||len==0) return 0;
    for(;;){
        char c=uart_getc();
        if(c=='\r'||c=='\n'){ uart_putc('\n'); break; }
        if((c==0x7f||c=='\b')){           /* backspace */
            if(i>0){ i--; uart_puts("\b \b"); }
            continue;
        }
        if(i<len-1){
            buf[i++]=c;
            uart_putc(echo_mask?echo_mask:c);
        }
    }
    buf[i]='\0';
    return i;
}
