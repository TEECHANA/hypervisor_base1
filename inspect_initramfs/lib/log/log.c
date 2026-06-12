#include "log.h"
#include "../../include/types.h"
extern void uart_putc(char);
extern void uart_puts(const char*);
extern void uart_puthex(u64);
extern void uart_putdec(s64);
static const char*_tags[]={"","[ERR]","[WRN]","[INF]","[DBG]"};
static void _fmt(const char*f,__builtin_va_list ap){
    for(;*f;f++){
        if(*f!='%'){uart_putc(*f);continue;}
        char spec=*++f; int lng=0;
        if(spec=='l'){lng=1;spec=*++f;}
        switch(spec){
        case 's':{const char*s=__builtin_va_arg(ap,const char*);uart_puts(s?s:"(null)");break;}
        case 'd':{s64 v=lng?__builtin_va_arg(ap,s64):(s64)__builtin_va_arg(ap,s32);uart_putdec(v);break;}
        case 'u':{u64 v=lng?__builtin_va_arg(ap,u64):(u64)__builtin_va_arg(ap,u32);uart_putdec((s64)v);break;}
        case 'x':case 'p':{u64 v=lng?__builtin_va_arg(ap,u64):(u64)__builtin_va_arg(ap,u32);uart_puthex(v);break;}
        case 'c':{uart_putc((char)__builtin_va_arg(ap,int));break;}
        case '%':uart_putc('%');break;
        default:uart_putc('%');uart_putc(spec);
        }
    }
}
void _hyp_log(int lvl,const char*fmt,...){
    if(lvl<1||lvl>4)return;
    uart_puts("HYP ");uart_puts(_tags[lvl]);uart_puts(" ");
    __builtin_va_list ap;__builtin_va_start(ap,fmt);
    _fmt(fmt,ap);__builtin_va_end(ap);
    uart_putc('\n');
}
