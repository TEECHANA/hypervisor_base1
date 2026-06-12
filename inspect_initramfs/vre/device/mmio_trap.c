#include "mmio_trap.h"
#include "../../include/config.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

typedef struct { u64 base; u64 sz; mmio_fn fn; void *priv; bool active; } mmio_reg_t;
static mmio_reg_t _regs[MAX_MMIO_REGIONS];
static u32 _nr = 0;

err_t mmio_register(u64 base, u64 sz, mmio_fn fn, void *priv){
    if(_nr>=MAX_MMIO_REGIONS) return E_NOMEM;
    _regs[_nr++]=(mmio_reg_t){base,sz,fn,priv,true};
    LOG_DEBUG("MMIO emul: 0x%lx-0x%lx", base, base+sz);
    return E_OK;
}

void mmio_handle(u64 fa, u64 esr, void *regs){
    UNUSED(regs);
    bool wr=(bool)((esr>>6)&1);
    u64 val=0;
    for(u32 i=0;i<_nr;i++){
        if(!_regs[i].active) continue;
        if(fa>=_regs[i].base && fa<_regs[i].base+_regs[i].sz){
            if(_regs[i].fn) _regs[i].fn(fa,wr,&val,_regs[i].priv);
            return;
        }
    }
    LOG_WARN("MMIO: unhandled %s @ 0x%lx", wr?"WR":"RD", fa);
}
