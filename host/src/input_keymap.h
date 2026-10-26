// input_keymap.h - USB HID usage (= SDL_Scancode) -> Linux KEY_* code, for the front-ends that get HID
// scancodes (SDL on the console). 0 = no Linux equivalent. The table is positional like the codes
// themselves: it names the physical key, not the character it types.
#pragma once
#include <stdint.h>

static inline uint16_t xrl_hid_to_linux_key(unsigned hid) {
    static const uint16_t kHid4[] = {    // HID 4..29: A..Z
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    static const uint16_t kHid30[] = {   // HID 30..39: 1..9, 0
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const uint16_t kHid40[] = {   // HID 40..: Enter Esc Backspace Tab Space - = [ ] \ #~ ; ' ` , . /
        28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53};
    static const uint16_t kHid57[] = {   // HID 57..: CapsLock F1..F12 PrtSc ScrLk Pause Ins Home PgUp Del End PgDn Right Left Down Up NumLock
        58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, 69};
    static const uint16_t kHid84[] = {   // HID 84..: KP / * - + Enter 1..9 0 .  then \| (non-US) Application
        98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127};
    static const uint16_t kHid224[] = {  // HID 224..231: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui
        29, 42, 56, 125, 97, 54, 100, 126};
    if (hid >= 4 && hid < 4 + sizeof kHid4 / 2) return kHid4[hid - 4];
    if (hid >= 30 && hid < 30 + sizeof kHid30 / 2) return kHid30[hid - 30];
    if (hid >= 40 && hid < 40 + sizeof kHid40 / 2) return kHid40[hid - 40];
    if (hid >= 57 && hid < 57 + sizeof kHid57 / 2) return kHid57[hid - 57];
    if (hid >= 84 && hid < 84 + sizeof kHid84 / 2) return kHid84[hid - 84];
    if (hid >= 224 && hid < 224 + sizeof kHid224 / 2) return kHid224[hid - 224];
    return 0;
}
