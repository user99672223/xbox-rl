/* A Windows (PE) program that exercises Wine's xbox-rl forwarders against the fake host in
 * wine/tests/musl-smoke/host_ops.c.  Built with mingw, run by tests/run-pe-test.sh.
 * It talks to the DLLs exactly as a game would: LoadLibrary + GetProcAddress, no Wine headers. */
#include <windows.h>
#include <stdio.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } XSTATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } VIB;
typedef struct { BYTE Type, SubType; WORD Flags; PAD Gamepad; VIB Vibration; } CAPS;

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    CHECK(x != NULL);
    if (x)
    {
        DWORD (WINAPI *get)(DWORD, XSTATE *) = (void *)GetProcAddress(x, "XInputGetState");
        DWORD (WINAPI *set)(DWORD, VIB *) = (void *)GetProcAddress(x, "XInputSetState");
        DWORD (WINAPI *caps)(DWORD, DWORD, CAPS *) = (void *)GetProcAddress(x, "XInputGetCapabilities");
        XSTATE st = {0}, st2 = {0}; VIB v = { 100, 200 }; CAPS c = {0};
        CHECK(get && set && caps);
        CHECK(get(0, &st) == ERROR_SUCCESS);
        printf("pad0: buttons=%04x lt=%u rt=%u lx=%d ly=%d packet=%lu\n", st.Gamepad.wButtons,
               st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
               (unsigned long)st.dwPacketNumber);
        CHECK(st.Gamepad.wButtons == 0x1001);
        CHECK(st.Gamepad.bLeftTrigger == 200 && st.Gamepad.bRightTrigger == 7);
        CHECK(st.Gamepad.sThumbLX == 12345 && st.Gamepad.sThumbLY == -321);
        CHECK(get(0, &st2) == ERROR_SUCCESS && st2.dwPacketNumber == st.dwPacketNumber);   /* unchanged -> same packet */
        CHECK(get(1, &st) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(get(4, &st) == ERROR_BAD_ARGUMENTS);
        CHECK(set(0, &v) == ERROR_SUCCESS);
        CHECK(set(1, &v) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(caps(0, 0, &c) == ERROR_SUCCESS && c.SubType == 1 && c.Gamepad.wButtons != 0);
    }

    HMODULE d11 = LoadLibraryA("d3d11.dll");
    CHECK(d11 != NULL);
    if (d11)
    {
        HRESULT (WINAPI *create)(void *, int, HMODULE, UINT, const int *, UINT, UINT, void **, int *, void **) =
            (void *)GetProcAddress(d11, "D3D11CreateDevice");
        void *dev = NULL, *ctx = NULL; int fl = 0;
        HRESULT hr;
        CHECK(create != NULL);
        hr = create(NULL, 1 /* HARDWARE */, NULL, 0, NULL, 0, 7, &dev, &fl, &ctx);
        printf("D3D11CreateDevice hr=%08lx dev=%p fl=%#x ctx=%p\n", (unsigned long)hr, dev, fl, ctx);
        CHECK(hr == S_OK);
        CHECK(dev && ((ULONGLONG *)dev)[1] == 0x0D3D11DEull);   /* the host's object reaches the game untouched */
        CHECK(ctx && ((ULONGLONG *)ctx)[1] == 0x0D3D11C7ull);
        CHECK(fl == 0xb000);                              /* 11_0: console cap */
        CHECK(create(NULL, 5 /* WARP */, NULL, 0, NULL, 0, 7, &dev, &fl, &ctx) == (HRESULT)0x887a0004);
    }

    HMODULE d12 = LoadLibraryA("d3d12.dll");
    CHECK(d12 != NULL);
    if (d12)
    {
        HRESULT (WINAPI *create)(void *, int, const GUID *, void **) = (void *)GetProcAddress(d12, "D3D12CreateDevice");
        GUID iid = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
        void *dev = NULL;
        CHECK(create != NULL);
        CHECK(create(NULL, 0xb000, &iid, &dev) == S_OK);
        CHECK(dev && ((ULONGLONG *)dev)[1] == 0xD3D12DE5ull);
        CHECK(create(NULL, 0xc100 /* 12_1: above the console */, &iid, &dev) == E_INVALIDARG);
    }

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
