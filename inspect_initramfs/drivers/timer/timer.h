#ifndef HYP_TIMER_H
#define HYP_TIMER_H
#include "../../include/types.h"
void timer_set_us(u32 microseconds);
void timer_disable(void);
u64  timer_now_us(void);
#endif
