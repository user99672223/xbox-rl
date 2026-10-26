// inq_test.cpp - self-test of the host's keyboard/mouse event queue (host/src/input_queue.cpp) and the SDL
// scancode map (host/src/input_keymap.h): synthetic events go in through the window-system side and must
// arrive, in order and with the documented transformations, through the xrl_input_ops slots the guest uses.
// Build target xrl_inq_test; exit code = number of failed checks.
#include <windows.h>
#include <cstdio>
#include <thread>
#include "input_queue.h"
#include "input_keymap.h"

static int fails;
#define CHECK(c, name) do { if (c) printf("  ok   %s\n", name); else { printf("  FAIL %s\n", name); fails++; } } while (0)

static void drain_all() { xrl_input_event b[64]; while (xrl_inq_read_events(b, 64)) {} }

int main() {
    xrl_input_event ev[64];

    // keys: press, repeat, release; a release of a key that is not held is dropped
    xrl_inq_push(XRL_IEV_KEY, 30, 1);
    xrl_inq_push(XRL_IEV_KEY, 30, 1);
    xrl_inq_push(XRL_IEV_KEY, 30, 0);
    xrl_inq_push(XRL_IEV_KEY, 30, 0);
    uint32_t n = xrl_inq_read_events(ev, 64);
    CHECK(n == 3 && ev[0].type == XRL_IEV_KEY && ev[0].code == 30 && ev[0].value == 1 && ev[1].value == 2 && ev[2].value == 0,
          "press, auto-repeat (2) and release; the stray release is dropped");
    CHECK(ev[0].time_ns != 0 && ev[2].time_ns >= ev[0].time_ns, "events carry increasing host timestamps");
    CHECK(xrl_inq_read_events(ev, 64) == 0, "the queue is empty after reading");

    // relative motion: same-axis neighbours are summed, different axes are not
    xrl_inq_push(XRL_IEV_REL, 0, 3);
    xrl_inq_push(XRL_IEV_REL, 0, 4);
    xrl_inq_push(XRL_IEV_REL, 1, -2);
    xrl_inq_push(XRL_IEV_REL, 0, 1);
    n = xrl_inq_read_events(ev, 64);
    CHECK(n == 3 && ev[0].value == 7 && ev[1].code == 1 && ev[1].value == -2 && ev[2].value == 1, "REL_X 3+4 coalesce to 7; X, Y, X stay three events");

    // partial reads keep the rest, oldest first
    for (int i = 0; i < 5; i++) { xrl_inq_push(XRL_IEV_KEY, 2 + i, 1); }
    n = xrl_inq_read_events(ev, 2);
    CHECK(n == 2 && ev[0].code == 2 && ev[1].code == 3, "read(max=2) returns the two oldest");
    n = xrl_inq_read_events(ev, 64);
    CHECK(n == 3 && ev[0].code == 4 && ev[2].code == 6, "the rest follows");
    for (int i = 0; i < 5; i++) xrl_inq_push(XRL_IEV_KEY, 2 + i, 0);
    drain_all();

    // focus loss releases held keys, then reports FOCUS 0
    xrl_inq_push(XRL_IEV_KEY, 42, 1);
    xrl_inq_push(XRL_IEV_KEY, 0x110, 1);
    drain_all();
    xrl_inq_focus_lost();
    n = xrl_inq_read_events(ev, 64);
    CHECK(n == 3 && ev[0].type == XRL_IEV_KEY && ev[0].value == 0 && ev[1].type == XRL_IEV_KEY && ev[1].value == 0 &&
          ev[2].type == XRL_IEV_FOCUS && ev[2].value == 0, "focus loss: held key and button released, then FOCUS 0");
    xrl_inq_push(XRL_IEV_KEY, 42, 0);
    CHECK(xrl_inq_read_events(ev, 64) == 0, "...and they are no longer held");

    // wait_events: timeout when empty, immediate when queued, woken by another thread
    DWORD t0 = GetTickCount();
    CHECK(xrl_inq_wait_events(120) == XRL_ERR_NOTAVAIL && GetTickCount() - t0 >= 100, "wait_events times out on an empty queue");
    xrl_inq_push(XRL_IEV_TEXT, 0, 0x263A);
    t0 = GetTickCount();
    CHECK(xrl_inq_wait_events(5000) == XRL_OK && GetTickCount() - t0 < 500, "wait_events returns at once when events are queued");
    n = xrl_inq_read_events(ev, 64);
    CHECK(n == 1 && ev[0].type == XRL_IEV_TEXT && ev[0].value == 0x263A, "TEXT carries the code point");
    std::thread th([] { Sleep(80); xrl_inq_push(XRL_IEV_KEY, 57, 1); });
    t0 = GetTickCount();
    CHECK(xrl_inq_wait_events(0xFFFFFFFFu) == XRL_OK && GetTickCount() - t0 >= 50, "an infinite wait is released by a push from another thread");
    th.join();
    drain_all();
    xrl_inq_push(XRL_IEV_KEY, 57, 0);
    drain_all();

    // ABS mapping: whole window, then an aspect-fitted picture
    int32_t ax, ay;
    xrl_inq_abs_from_client(0, 0, 1000, 1000, &ax, &ay);
    CHECK(ax == 0 && ay == 0, "ABS: the window's top-left is (0,0)");
    xrl_inq_abs_from_client(500, 500, 1000, 1000, &ax, &ay);
    CHECK(ax >= 16383 && ax <= 16385 && ay >= 16383 && ay <= 16385, "ABS: the window's centre is half range");
    xrl_inq_abs_from_client(1000, 1000, 1000, 1000, &ax, &ay);
    CHECK(ax == XRL_ABS_MAX && ay == XRL_ABS_MAX, "ABS: the bottom-right is XRL_ABS_MAX");
    CHECK(xrl_inq_set_content_size(1280, 720) == XRL_OK, "set_content_size accepted");
    xrl_inq_abs_from_client(500, 500, 1000, 1000, &ax, &ay);   // a 1000x562.5 picture centred vertically (bars of 218.75 px)
    CHECK(ax >= 16383 && ax <= 16385 && ay >= 16383 && ay <= 16385, "ABS in a 1000x1000 window with a 16:9 picture: the centre maps to the centre");
    xrl_inq_abs_from_client(500, 100, 1000, 1000, &ax, &ay);
    CHECK(ax >= 16383 && ay == 0, "ABS: a position in the top letterbox bar clamps to the picture's top edge");
    xrl_inq_abs_from_client(1000, 900, 1000, 1000, &ax, &ay);
    CHECK(ax == XRL_ABS_MAX && ay == XRL_ABS_MAX, "ABS: below-right of the picture clamps to the maximum");
    xrl_inq_abs_from_client(250, 500, 1000, 1000, &ax, &ay);
    CHECK(ax >= 8191 && ax <= 8193 && ay >= 16383 && ay <= 16385, "ABS: a quarter of the way across the picture is a quarter of the range");
    xrl_inq_set_content_size(0, 0);

    // mouse mode
    CHECK(xrl_inq_mouse_mode() == XRL_MOUSE_ABSOLUTE, "the mouse starts in absolute mode");
    CHECK(xrl_inq_set_mouse_mode(XRL_MOUSE_RELATIVE) == XRL_OK && xrl_inq_mouse_mode() == XRL_MOUSE_RELATIVE, "set_mouse_mode(RELATIVE)");
    CHECK(xrl_inq_set_mouse_mode(7) != XRL_OK && xrl_inq_mouse_mode() == XRL_MOUSE_RELATIVE, "an invalid mode is refused");
    xrl_inq_set_mouse_mode(XRL_MOUSE_ABSOLUTE);

    // overflow: motion is dropped first, keys still get in
    uint64_t pushed0, dropped0, pushed1, dropped1;
    xrl_inq_stats(&pushed0, &dropped0);
    for (int i = 0; i < 6000; i++) xrl_inq_push(XRL_IEV_ABS, (uint16_t)(i & 1), i);
    xrl_inq_push(XRL_IEV_KEY, 1, 1);
    xrl_inq_stats(&pushed1, &dropped1);
    CHECK(dropped1 > dropped0 && pushed1 - pushed0 <= 3074, "a flood of motion is dropped at 3/4 full");
    uint32_t total = 0, last_is_key = 0;
    while ((n = xrl_inq_read_events(ev, 64)) != 0) { total += n; last_is_key = ev[n - 1].type == XRL_IEV_KEY; }
    CHECK(last_is_key && total <= 3074, "...but the key press behind the flood arrived");
    xrl_inq_push(XRL_IEV_KEY, 1, 0);
    drain_all();

    // the SDL (USB HID) scancode map
    CHECK(xrl_hid_to_linux_key(4) == 30 && xrl_hid_to_linux_key(29) == 44 && xrl_hid_to_linux_key(20) == 16, "HID A, Z, Q -> KEY_A 30, KEY_Z 44, KEY_Q 16");
    CHECK(xrl_hid_to_linux_key(30) == 2 && xrl_hid_to_linux_key(39) == 11, "HID 1, 0 -> KEY_1 2, KEY_0 11");
    CHECK(xrl_hid_to_linux_key(40) == 28 && xrl_hid_to_linux_key(41) == 1 && xrl_hid_to_linux_key(44) == 57, "HID Enter, Esc, Space -> 28, 1, 57");
    CHECK(xrl_hid_to_linux_key(58) == 59 && xrl_hid_to_linux_key(67) == 68 && xrl_hid_to_linux_key(68) == 87 && xrl_hid_to_linux_key(69) == 88, "HID F1, F10, F11, F12 -> 59, 68, 87, 88");
    CHECK(xrl_hid_to_linux_key(79) == 106 && xrl_hid_to_linux_key(80) == 105 && xrl_hid_to_linux_key(81) == 108 && xrl_hid_to_linux_key(82) == 103, "HID arrows -> KEY_RIGHT/LEFT/DOWN/UP 106/105/108/103");
    CHECK(xrl_hid_to_linux_key(224) == 29 && xrl_hid_to_linux_key(225) == 42 && xrl_hid_to_linux_key(230) == 100 && xrl_hid_to_linux_key(231) == 126, "HID LCtrl, LShift, RAlt, RGui -> 29, 42, 100, 126");
    CHECK(xrl_hid_to_linux_key(88) == 96 && xrl_hid_to_linux_key(98) == 82 && xrl_hid_to_linux_key(99) == 83, "HID KP Enter, KP0, KP. -> 96, 82, 83");
    CHECK(xrl_hid_to_linux_key(0) == 0 && xrl_hid_to_linux_key(200) == 0 && xrl_hid_to_linux_key(1000) == 0, "unmapped HID codes -> 0");
    int seen[256] = {}, dup = 0;
    for (unsigned h = 0; h < 232; h++) { unsigned k = xrl_hid_to_linux_key(h); if (k && seen[k]++) dup++; }
    CHECK(dup == 1, "the HID map is injective except HID 49/50 (both Linux KEY_BACKSLASH; one is the ISO #)");

    printf("TOTAL FAILS=%d\n", fails);
    return fails;
}
