#ifndef HYP_ERROR_H
#define HYP_ERROR_H
typedef enum {
    E_OK          =  0,
    E_GENERIC     = -1,
    E_NOMEM       = -2,
    E_INVAL       = -3,
    E_BUSY        = -4,
    E_NOTFOUND    = -5,
    E_DENIED      = -6,
    E_FAULT       = -7,
    E_UNSUPPORTED = -8,
    E_TIMEOUT     = -9,
} err_t;
#define OK(e)   ((e)==E_OK)
#define FAIL(e) ((e)!=E_OK)
#endif
