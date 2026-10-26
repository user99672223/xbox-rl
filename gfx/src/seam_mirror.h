// seam_mirror.h — byte-for-byte mirror of the xrl_gfx_host_services struct that
// Workstream A defines in host/src/gfx_seam.h (the host<->gfx seam). gfx/ may not
// include host/ headers (gfx must build standalone), so the layout is copied here.
// KEEP IDENTICAL to host/src/gfx_seam.h; if A changes it, this must follow.
// (The public gfx header only forward-declares the struct, so a TU that includes
// both host/src/gfx_seam.h and xrl_gfx.h still compiles.)
#pragma once
#include "xrl_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xrl_gfx_host_services {
    uint32_t struct_size;
    xrl_log_fn log;
    void *(*get_core_window)(void);
    void  (*get_window_size)(uint32_t *w, uint32_t *h);
    int   (*run_on_gfx_thread)(const char *what, void (*fn)(void *), void *ctx,
                               uint32_t timeout_ms);
    int   (*on_gfx_thread)(void);
} xrl_gfx_host_services;

#ifdef __cplusplus
}
#endif
