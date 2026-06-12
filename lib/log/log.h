#ifndef HYP_LOG_H
#define HYP_LOG_H
#include "../../include/config.h"
#define LL_ERROR 1
#define LL_WARN  2
#define LL_INFO  3
#define LL_DEBUG 4
void _hyp_log(int lvl, const char *fmt, ...);
#if LOG_LEVEL>=LL_ERROR
#define LOG_ERROR(f,...) _hyp_log(LL_ERROR,f,##__VA_ARGS__)
#else
#define LOG_ERROR(f,...) do{}while(0)
#endif
#if LOG_LEVEL>=LL_WARN
#define LOG_WARN(f,...)  _hyp_log(LL_WARN, f,##__VA_ARGS__)
#else
#define LOG_WARN(f,...)  do{}while(0)
#endif
#if LOG_LEVEL>=LL_INFO
#define LOG_INFO(f,...)  _hyp_log(LL_INFO, f,##__VA_ARGS__)
#else
#define LOG_INFO(f,...)  do{}while(0)
#endif
#if LOG_LEVEL>=LL_DEBUG
#define LOG_DEBUG(f,...) _hyp_log(LL_DEBUG,f,##__VA_ARGS__)
#else
#define LOG_DEBUG(f,...) do{}while(0)
#endif
#endif
