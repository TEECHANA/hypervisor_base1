#ifndef HYP_TYPES_H
#define HYP_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;
typedef u64                 size_t;
typedef u64                 uintptr_t;
typedef s64                 intptr_t;
typedef u64                 paddr_t;   /* physical address          */
typedef u64                 vaddr_t;   /* virtual  address          */
typedef u64                 ipa_t;     /* intermediate physical (guest PA) */
typedef u8                  bool;
#define true  ((bool)1)
#define false ((bool)0)
#ifndef NULL
#define NULL ((void*)0)
#endif

#define BIT(n)            (1ULL<<(n))
#define ARRAY_SIZE(a)     (sizeof(a)/sizeof((a)[0]))
#define ALIGN_UP(x,a)     (((u64)(x)+(u64)((a)-1))&~(u64)((a)-1))
#define ALIGN_DOWN(x,a)   ((u64)(x)&~(u64)((a)-1))
#define IS_ALIGNED(x,a)   (!((u64)(x)&(u64)((a)-1)))
#define MIN(a,b)          ((a)<(b)?(a):(b))
#define MAX(a,b)          ((a)>(b)?(a):(b))
#define UNUSED(x)         ((void)(x))
#define container_of(p,t,m) ((t*)((u8*)(p)-__builtin_offsetof(t,m)))

#define __aligned(n)  __attribute__((aligned(n)))
#define __packed      __attribute__((packed))
#define __noreturn    __attribute__((noreturn))
#define __weak        __attribute__((weak))
#define __section(s)  __attribute__((section(s)))
#define __used        __attribute__((used))

#define PAGE_SIZE   4096ULL
#define PAGE_BITS   12
#define PAGE_MASK   (~(PAGE_SIZE-1))

#endif /* HYP_TYPES_H */
