#include "irq_router.h"
#include "../../include/config.h"
#include "../../drivers/gic/gicv3.h"
#include "../../lib/log/log.h"
#include "../../lib/str/string.h"

typedef struct { u32 phys; u32 vm_id; u32 virq; bool active; } route_t;
static route_t _routes[MAX_IRQ_ROUTES];
static u32 _nr = 0;

err_t irq_router_init(void){
    memset(_routes,0,sizeof(_routes)); _nr=0; return E_OK;}

err_t irq_route_add(u32 phys, u32 vm_id, u32 virq){
    if(_nr>=MAX_IRQ_ROUTES) return E_NOMEM;
    _routes[_nr++]=(route_t){phys,vm_id,virq,true};
    LOG_DEBUG("IRQ route: phys=%d -> VM%d virq=%d", phys, vm_id, virq);
    return E_OK;
}

err_t irq_route_remove(u32 phys){
    for(u32 i=0;i<_nr;i++) if(_routes[i].phys==phys){_routes[i].active=false;return E_OK;}
    return E_NOTFOUND;
}

void irq_route_to_vm(u32 phys){
    for(u32 i=0;i<_nr;i++){
        if(_routes[i].active && _routes[i].phys==phys){
            LOG_DEBUG("Route IRQ%d -> VM%d vIRQ%d",phys,_routes[i].vm_id,_routes[i].virq);
            gic_inject_virq(_routes[i].virq, 0x80);
            return;
        }
    }
    LOG_WARN("No route for IRQ %d", phys);
}
