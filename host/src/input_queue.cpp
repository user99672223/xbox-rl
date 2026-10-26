// input_queue.cpp - keyboard/mouse event queue behind xrl_input_ops (ABI v3).
//
// One bounded ring under a mutex; the window-system thread pushes, a guest thread waits on the
// condition variable and drains. The queue is the only place that knows which keys are held, so
// key repeat and focus loss are handled here and every front-end gets them the same way.
#include "pch.h"
#include "input_queue.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

constexpr uint32_t kCap = 4096;       // events
constexpr uint32_t kMotionCap = kCap * 3 / 4;   // above this, motion is dropped to keep room for keys
constexpr int kKeys = 0x300;          // KEY_* and BTN_* codes tracked as held

struct Queue {
    std::mutex m;
    std::condition_variable cv;
    xrl_input_event ring[kCap];
    uint32_t head = 0, count = 0;
    bool held[kKeys] = {};
    uint32_t mode = XRL_MOUSE_ABSOLUTE;
    uint32_t content_w = 0, content_h = 0;
    uint64_t pushed = 0, dropped = 0;
};
Queue& Q() { static Queue* q = new Queue(); return *q; }

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// q.m held
void put_locked(Queue& q, uint16_t type, uint16_t code, int32_t value) {
    if (type == XRL_IEV_REL && q.count) {   // coalesce with the newest event if it is the same axis
        xrl_input_event& t = q.ring[(q.head + q.count - 1) % kCap];
        if (t.type == XRL_IEV_REL && t.code == code) { t.value += value; t.time_ns = now_ns(); return; }
    }
    if (q.count >= kCap || (q.count >= kMotionCap && (type == XRL_IEV_REL || type == XRL_IEV_ABS))) { q.dropped++; return; }
    xrl_input_event& e = q.ring[(q.head + q.count) % kCap];
    e.type = type; e.code = code; e.value = value; e.time_ns = now_ns();
    q.count++;
    q.pushed++;
}

}  // namespace

extern "C" {

void xrl_inq_push(uint16_t type, uint16_t code, int32_t value) {
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> lk(q.m);
        if (type == XRL_IEV_KEY) {
            if (code >= kKeys) return;
            if (value) { value = q.held[code] ? 2 : 1; q.held[code] = true; }
            else { if (!q.held[code]) return; q.held[code] = false; }
        }
        put_locked(q, type, code, value);
    }
    q.cv.notify_all();
}

void xrl_inq_focus_lost(void) {
    Queue& q = Q();
    {
        std::lock_guard<std::mutex> lk(q.m);
        for (int c = 0; c < kKeys; c++)
            if (q.held[c]) { q.held[c] = false; put_locked(q, XRL_IEV_KEY, (uint16_t)c, 0); }
        put_locked(q, XRL_IEV_FOCUS, 0, 0);
    }
    q.cv.notify_all();
}

uint32_t xrl_inq_mouse_mode(void) { Queue& q = Q(); std::lock_guard<std::mutex> lk(q.m); return q.mode; }

void xrl_inq_content_size(uint32_t* w, uint32_t* h) {
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.m);
    *w = q.content_w; *h = q.content_h;
}

void xrl_inq_abs_from_client(int cx, int cy, int client_w, int client_h, int32_t* ax, int32_t* ay) {
    uint32_t cw, ch;
    xrl_inq_content_size(&cw, &ch);
    double x0 = 0, y0 = 0, w = client_w, h = client_h;
    if (cw && ch && client_w > 0 && client_h > 0) {   // the picture, aspect-fitted and centred
        double s = (double)client_w / cw < (double)client_h / ch ? (double)client_w / cw : (double)client_h / ch;
        w = cw * s; h = ch * s;
        x0 = (client_w - w) / 2; y0 = (client_h - h) / 2;
    }
    double fx = w > 0 ? (cx - x0) / w : 0, fy = h > 0 ? (cy - y0) / h : 0;
    if (fx < 0) fx = 0; else if (fx > 1) fx = 1;
    if (fy < 0) fy = 0; else if (fy > 1) fy = 1;
    *ax = (int32_t)(fx * XRL_ABS_MAX + 0.5);
    *ay = (int32_t)(fy * XRL_ABS_MAX + 0.5);
}

uint32_t xrl_inq_read_events(xrl_input_event* buf, uint32_t max) {
    if (!buf) return 0;
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.m);
    uint32_t n = q.count < max ? q.count : max;
    for (uint32_t i = 0; i < n; i++) buf[i] = q.ring[(q.head + i) % kCap];
    q.head = (q.head + n) % kCap;
    q.count -= n;
    return n;
}

xrl_status xrl_inq_wait_events(uint32_t timeout_ms) {
    Queue& q = Q();
    std::unique_lock<std::mutex> lk(q.m);
    if (timeout_ms == 0xFFFFFFFFu) { q.cv.wait(lk, [&] { return q.count != 0; }); return XRL_OK; }
    return q.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return q.count != 0; }) ? XRL_OK : XRL_ERR_NOTAVAIL;
}

xrl_status xrl_inq_set_mouse_mode(uint32_t mode) {
    if (mode != XRL_MOUSE_ABSOLUTE && mode != XRL_MOUSE_RELATIVE) return XRL_ERR_GENERIC;
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.m);
    q.mode = mode;
    return XRL_OK;
}

xrl_status xrl_inq_set_content_size(uint32_t w, uint32_t h) {
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.m);
    q.content_w = w; q.content_h = h;
    return XRL_OK;
}

void xrl_inq_stats(uint64_t* pushed, uint64_t* dropped) {
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.m);
    if (pushed) *pushed = q.pushed;
    if (dropped) *dropped = q.dropped;
}

}  // extern "C"
