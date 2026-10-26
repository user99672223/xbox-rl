// input_queue.h - the keyboard/mouse event queue behind xrl_input_ops (ABI v3).
//
// Whoever owns the window (SDL on the console, a Win32 window on the test host) pushes events with
// xrl_inq_push(); the guest drains them through the ops below. Plain C linkage, no WinRT/SDL types.
#pragma once
#include "xrl_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- window-system side (any thread) ----
// Queue one event. KEY presses of a held key become autorepeats (value 2), releases of a key that is
// not held are dropped, consecutive REL motions are summed; when the queue is nearly full only
// KEY/TEXT/FOCUS events are kept (motion is dropped first). time_ns 0 = now.
void xrl_inq_push(uint16_t type, uint16_t code, int32_t value);
// Release every held key and button (the window lost focus), then queue FOCUS 0.
void xrl_inq_focus_lost(void);
// The mode the guest asked for (XRL_MOUSE_*) and the size of the picture it shows (0,0 = whole window);
// the window system applies the first (cursor capture) and uses the second to normalize ABS positions.
uint32_t xrl_inq_mouse_mode(void);
void     xrl_inq_content_size(uint32_t* w, uint32_t* h);
// Map a window-client position to ABS_X/ABS_Y values (0..XRL_ABS_MAX), aspect-fitting the content size
// into the client area the way the host's presenter does. Positions in the bars clamp to the picture.
void xrl_inq_abs_from_client(int cx, int cy, int client_w, int client_h, int32_t* ax, int32_t* ay);

// ---- the xrl_input_ops (v3) slots ----
uint32_t   xrl_inq_read_events(xrl_input_event* buf, uint32_t max);
xrl_status xrl_inq_wait_events(uint32_t timeout_ms);
xrl_status xrl_inq_set_mouse_mode(uint32_t mode);
xrl_status xrl_inq_set_content_size(uint32_t w, uint32_t h);

// Events queued and dropped since start (diagnostics / self-test).
void xrl_inq_stats(uint64_t* pushed, uint64_t* dropped);

#ifdef __cplusplus
}
#endif
