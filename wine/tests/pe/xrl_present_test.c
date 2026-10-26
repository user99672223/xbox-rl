/* xrl_present_test.c -- a Windows D3D11 program that makes a DXGI swapchain for its window and
 * presents frames.  Built with mingw (wine/tests/build-pe-tests.sh), run as the main program of
 * Wine on the layer:
 *
 *   xrl_present_test.exe [--only N] [--hold SECONDS] [--frames N] [--list]
 *
 * For every configuration of the matrix below it creates the swapchain, clears the back buffer to
 * a new colour each frame and presents it, reads one pixel back through D3D11 each time, resizes
 * the buffers once, and prints what it did.  The last frames of every configuration are one fixed
 * colour (FINAL_*), so a screenshot of the host window can be compared with it.  `--only N` runs
 * configuration N alone and `--hold` keeps presenting that colour for that many seconds; with
 * `--pattern` the held picture is four quadrants instead (top-left red, top-right green,
 * bottom-left blue, bottom-right yellow, uploaded with UpdateSubresource in the buffer's own channel
 * order) so a screenshot also shows that nothing is flipped or channel-swapped.
 *
 * Exit code: 0 when every required configuration worked.  Configuration 3 asks for the factory
 * through the device (IDXGIDevice -> adapter -> parent): on a D3D11On12 device that yields the
 * host's own factory, so it is informational, not required.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FINAL_R 51
#define FINAL_G 153
#define FINAL_B 255

enum api { API_FACTORY, API_FACTORY2_HWND, API_DEVICE_AND_SWAPCHAIN, API_FACTORY_VIA_DEVICE };

struct config
{
    const char *name;
    enum api api;
    DXGI_SWAP_EFFECT effect;
    DXGI_FORMAT format;
    UINT buffers;
    int required;
};

static const struct config configs[] =
{
    { "CreateSwapChain discard BGRA",       API_FACTORY,              DXGI_SWAP_EFFECT_DISCARD,         DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1 },
    { "CreateSwapChain discard RGBA",       API_FACTORY,              DXGI_SWAP_EFFECT_DISCARD,         DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1 },
    { "CreateSwapChain flip_discard BGRA",  API_FACTORY,              DXGI_SWAP_EFFECT_FLIP_DISCARD,    DXGI_FORMAT_B8G8R8A8_UNORM, 2, 1 },
    { "CreateSwapChain flip_seq RGBA",      API_FACTORY,              DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_FORMAT_R8G8B8A8_UNORM, 3, 1 },
    { "ForHwnd flip_discard RGBA",          API_FACTORY2_HWND,        DXGI_SWAP_EFFECT_FLIP_DISCARD,    DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1 },
    { "ForHwnd flip_discard BGRA",          API_FACTORY2_HWND,        DXGI_SWAP_EFFECT_FLIP_DISCARD,    DXGI_FORMAT_B8G8R8A8_UNORM, 2, 1 },
    { "ForHwnd flip_seq BGRA",              API_FACTORY2_HWND,        DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_FORMAT_B8G8R8A8_UNORM, 3, 1 },
    { "ForHwnd discard RGBA",               API_FACTORY2_HWND,        DXGI_SWAP_EFFECT_DISCARD,         DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1 },
    { "D3D11CreateDeviceAndSwapChain",      API_DEVICE_AND_SWAPCHAIN, DXGI_SWAP_EFFECT_DISCARD,         DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1 },
    { "factory via device discard BGRA",    API_FACTORY_VIA_DEVICE,   DXGI_SWAP_EFFECT_DISCARD,         DXGI_FORMAT_B8G8R8A8_UNORM, 1, 0 },
};
#define NCONFIG (sizeof(configs) / sizeof(configs[0]))

static int frames_per_config = 40;
static int pattern;
static HWND window;

#define STEP(...) do { printf("  " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAILED_STEP(what, hr) do { printf("  FAIL %s: hr=%#lx\n", what, (unsigned long)(hr)); fflush(stdout); goto done; } while (0)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND make_window(UINT w, UINT h)
{
    WNDCLASSW wc = {0};
    RECT r = { 0, 0, (LONG)w, (LONG)h };
    HWND hwnd;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"xrl_present_test";
    RegisterClassW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowW(wc.lpszClassName, L"xrl present test", WS_OVERLAPPEDWINDOW, 0, 0,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (hwnd) ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

struct frame_state
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *sc;
    ID3D11Texture2D *back;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *staging;
    DXGI_FORMAT format;
};

static HRESULT bind_back_buffer(struct frame_state *f)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (FAILED(hr = IDXGISwapChain_GetBuffer(f->sc, 0, &IID_ID3D11Texture2D, (void **)&f->back))) return hr;
    ID3D11Texture2D_GetDesc(f->back, &td);
    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(f->dev, (ID3D11Resource *)f->back, NULL, &f->rtv))) return hr;
    td.Width = 1; td.Height = 1; td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    if (f->staging) { ID3D11Texture2D_Release(f->staging); f->staging = NULL; }
    return ID3D11Device_CreateTexture2D(f->dev, &td, NULL, &f->staging);
}

static void unbind_back_buffer(struct frame_state *f)
{
    if (f->rtv) { ID3D11RenderTargetView_Release(f->rtv); f->rtv = NULL; }
    if (f->back) { ID3D11Texture2D_Release(f->back); f->back = NULL; }
    ID3D11DeviceContext_ClearState(f->ctx);
    ID3D11DeviceContext_Flush(f->ctx);
}

/* Read the centre pixel of the back buffer; returns it as 0xAARRGGBB regardless of the format. */
static HRESULT read_pixel(struct frame_state *f, UINT w, UINT h, UINT *argb)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box = { w / 2, h / 2, 0, w / 2 + 1, h / 2 + 1, 1 };
    HRESULT hr;
    const BYTE *p;

    ID3D11DeviceContext_CopySubresourceRegion(f->ctx, (ID3D11Resource *)f->staging, 0, 0, 0, 0,
                                              (ID3D11Resource *)f->back, 0, &box);
    if (FAILED(hr = ID3D11DeviceContext_Map(f->ctx, (ID3D11Resource *)f->staging, 0, D3D11_MAP_READ, 0, &m))) return hr;
    p = m.pData;
    if (f->format == DXGI_FORMAT_B8G8R8A8_UNORM) *argb = p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0];
    else *argb = p[3] << 24 | p[0] << 16 | p[1] << 8 | p[2];
    ID3D11DeviceContext_Unmap(f->ctx, (ID3D11Resource *)f->staging, 0);
    return S_OK;
}

static void frame_color(int i, int total, float c[4])
{
    if (i >= total - 8) { c[0] = FINAL_R / 255.0f; c[1] = FINAL_G / 255.0f; c[2] = FINAL_B / 255.0f; c[3] = 1.0f; return; }
    c[0] = (float)((i * 37) % 256) / 255.0f;
    c[1] = (float)((i * 91 + 60) % 256) / 255.0f;
    c[2] = (float)((i * 13 + 120) % 256) / 255.0f;
    c[3] = 1.0f;
}

/* four quadrants, as R,G,B logical colours, laid out in the swapchain format's byte order */
static void upload_pattern(struct frame_state *f, UINT w, UINT h)
{
    static const BYTE colors[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 } };
    BYTE *px = malloc((size_t)w * h * 4);
    UINT x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            const BYTE *c = colors[(y >= h / 2) * 2 + (x >= w / 2)];
            BYTE *d = px + ((size_t)y * w + x) * 4;
            if (f->format == DXGI_FORMAT_B8G8R8A8_UNORM) { d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; }
            else { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; }
            d[3] = 255;
        }
    ID3D11DeviceContext_UpdateSubresource(f->ctx, (ID3D11Resource *)f->back, 0, NULL, px, w * 4, 0);
    free(px);
}

static int run_frames(struct frame_state *f, UINT *w, UINT *h, int count, int *bad_pixels)
{
    int i;
    for (i = 0; i < count; i++)
    {
        float c[4];
        UINT px, want;
        HRESULT hr;

        frame_color(i, count, c);
        ID3D11DeviceContext_OMSetRenderTargets(f->ctx, 1, &f->rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(f->ctx, f->rtv, c);
        if (SUCCEEDED(read_pixel(f, *w, *h, &px)))
        {
            want = 0xff000000u | (UINT)(c[0] * 255.0f + 0.5f) << 16 | (UINT)(c[1] * 255.0f + 0.5f) << 8 | (UINT)(c[2] * 255.0f + 0.5f);
            if ((px & 0xffffff) != (want & 0xffffff)) { (*bad_pixels)++; if (*bad_pixels < 4) STEP("pixel %#x, expected %#x (frame %d)", px, want, i); }
        }
        else (*bad_pixels)++;
        if (FAILED(hr = IDXGISwapChain_Present(f->sc, 1, 0)))
        {
            printf("  FAIL Present frame %d: hr=%#lx\n", i, (unsigned long)hr);
            return 0;
        }
        pump();
    }
    return 1;
}

static int run_config(int index, ID3D11Device *shared_dev, ID3D11DeviceContext *shared_ctx, double hold)
{
    const struct config *cfg = &configs[index];
    struct frame_state f = {0};
    DXGI_SWAP_CHAIN_DESC desc = {0};
    DXGI_SWAP_CHAIN_DESC1 desc1 = {0};
    IDXGIFactory *factory = NULL;
    IDXGIFactory2 *factory2 = NULL;
    IDXGISwapChain1 *sc1 = NULL;
    UINT w = 640, h = 360;
    int ok = 0, bad_pixels = 0;
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC got;

    printf("[%d] %s\n", index, cfg->name);
    fflush(stdout);
    f.format = cfg->format;

    desc.BufferDesc.Width = w; desc.BufferDesc.Height = h;
    desc.BufferDesc.Format = cfg->format;
    desc.BufferDesc.RefreshRate.Numerator = 60; desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.BufferCount = cfg->buffers;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = cfg->effect;

    if (cfg->api == API_DEVICE_AND_SWAPCHAIN)
    {
        D3D_FEATURE_LEVEL fl;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                           &desc, &f.sc, &f.dev, &fl, &f.ctx);
        STEP("D3D11CreateDeviceAndSwapChain -> hr=%#lx FL=%#x", (unsigned long)hr, fl);
        if (FAILED(hr)) goto done;
    }
    else
    {
        f.dev = shared_dev; f.ctx = shared_ctx;
        ID3D11Device_AddRef(f.dev); ID3D11DeviceContext_AddRef(f.ctx);
        if (cfg->api == API_FACTORY_VIA_DEVICE)
        {
            IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL;
            if (FAILED(hr = ID3D11Device_QueryInterface(f.dev, &IID_IDXGIDevice, (void **)&dd))) FAILED_STEP("QI IDXGIDevice", hr);
            if (FAILED(hr = IDXGIDevice_GetAdapter(dd, &ad))) FAILED_STEP("IDXGIDevice::GetAdapter", hr);
            if (FAILED(hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&factory))) FAILED_STEP("IDXGIAdapter::GetParent", hr);
            IDXGIAdapter_Release(ad); IDXGIDevice_Release(dd);
        }
        else if (FAILED(hr = CreateDXGIFactory1(&IID_IDXGIFactory, (void **)&factory))) FAILED_STEP("CreateDXGIFactory1", hr);

        if (cfg->api == API_FACTORY2_HWND)
        {
            if (FAILED(hr = IDXGIFactory_QueryInterface(factory, &IID_IDXGIFactory2, (void **)&factory2))) FAILED_STEP("QI IDXGIFactory2", hr);
            desc1.Width = w; desc1.Height = h; desc1.Format = cfg->format;
            desc1.SampleDesc.Count = 1; desc1.BufferUsage = desc.BufferUsage;
            desc1.BufferCount = cfg->buffers; desc1.Scaling = DXGI_SCALING_STRETCH;
            desc1.SwapEffect = cfg->effect; desc1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(factory2, (IUnknown *)f.dev, window, &desc1, NULL, NULL, &sc1);
            STEP("CreateSwapChainForHwnd -> hr=%#lx", (unsigned long)hr);
            if (FAILED(hr)) goto done;
            if (FAILED(hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain, (void **)&f.sc))) FAILED_STEP("QI IDXGISwapChain", hr);
        }
        else
        {
            hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)f.dev, &desc, &f.sc);
            STEP("CreateSwapChain -> hr=%#lx", (unsigned long)hr);
            if (FAILED(hr)) goto done;
        }
    }

    if (SUCCEEDED(IDXGISwapChain_GetDesc(f.sc, &got)))
        STEP("GetDesc: %ux%u fmt=%u buffers=%u effect=%u", got.BufferDesc.Width, got.BufferDesc.Height,
             got.BufferDesc.Format, got.BufferCount, got.SwapEffect);
    if (FAILED(hr = bind_back_buffer(&f))) FAILED_STEP("GetBuffer/RTV", hr);
    STEP("back buffer %p, rtv %p", f.back, f.rtv);

    if (!run_frames(&f, &w, &h, frames_per_config / 2, &bad_pixels)) goto done;

    /* resize once: drop every reference to the buffers first */
    unbind_back_buffer(&f);
    w = 480; h = 270;
    hr = IDXGISwapChain_ResizeBuffers(f.sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    STEP("ResizeBuffers %ux%u -> hr=%#lx", w, h, (unsigned long)hr);
    if (FAILED(hr)) goto done;
    if (SUCCEEDED(IDXGISwapChain_GetDesc(f.sc, &got)))
        STEP("after resize: %ux%u buffers=%u", got.BufferDesc.Width, got.BufferDesc.Height, got.BufferCount);
    if (FAILED(hr = bind_back_buffer(&f))) FAILED_STEP("GetBuffer/RTV after resize", hr);

    if (!run_frames(&f, &w, &h, frames_per_config - frames_per_config / 2, &bad_pixels)) goto done;

    if (hold > 0)
    {
        DWORD end = GetTickCount() + (DWORD)(hold * 1000);
        float c[4] = { FINAL_R / 255.0f, FINAL_G / 255.0f, FINAL_B / 255.0f, 1.0f };
        printf("HOLD color=%d,%d,%d\n", FINAL_R, FINAL_G, FINAL_B);
        fflush(stdout);
        while ((LONG)(end - GetTickCount()) > 0)
        {
            if (pattern) upload_pattern(&f, w, h);
            else
            {
                ID3D11DeviceContext_OMSetRenderTargets(f.ctx, 1, &f.rtv, NULL);
                ID3D11DeviceContext_ClearRenderTargetView(f.ctx, f.rtv, c);
            }
            if (FAILED(hr = IDXGISwapChain_Present(f.sc, 1, 0))) FAILED_STEP("Present while holding", hr);
            pump();
        }
    }
    ok = bad_pixels == 0;
    STEP("%d frames presented, %d wrong read-back pixels", frames_per_config, bad_pixels);

done:
    if (f.rtv) ID3D11RenderTargetView_Release(f.rtv);
    if (f.back) ID3D11Texture2D_Release(f.back);
    if (f.staging) ID3D11Texture2D_Release(f.staging);
    if (f.sc) IDXGISwapChain_Release(f.sc);
    if (sc1) IDXGISwapChain1_Release(sc1);
    if (factory2) IDXGIFactory2_Release(factory2);
    if (factory) IDXGIFactory_Release(factory);
    if (f.ctx) { ID3D11DeviceContext_ClearState(f.ctx); ID3D11DeviceContext_Flush(f.ctx); ID3D11DeviceContext_Release(f.ctx); }
    if (f.dev) ID3D11Device_Release(f.dev);
    printf("[%d] %s: %s\n", index, cfg->name, ok ? "OK" : "FAILED");
    fflush(stdout);
    return ok;
}

int main(int argc, char **argv)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    int only = -1, i, required = 0, passed = 0;
    double hold = 0;
    HRESULT hr;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_per_config = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pattern")) pattern = 1;
        else if (!strcmp(argv[i], "--list"))
        {
            for (i = 0; i < (int)NCONFIG; i++) printf("%d %s\n", i, configs[i].name);
            return 0;
        }
    }
    if (frames_per_config < 10) frames_per_config = 10;

    window = make_window(640, 360);
    printf("window %p\n", window);
    if (!window) { printf("FAIL CreateWindow: %lu\n", GetLastError()); return 1; }

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice -> hr=%#lx FL=%#x device=%p\n", (unsigned long)hr, fl, dev);
    fflush(stdout);
    if (FAILED(hr)) return 1;

    for (i = 0; i < (int)NCONFIG; i++)
    {
        if (only >= 0 && i != only) continue;
        if (configs[i].required) required++;
        if (run_config(i, dev, ctx, only >= 0 ? hold : 0) && configs[i].required) passed++;
    }
    printf("RESULT: %d of %d required configurations passed\n", passed, required);
    fflush(stdout);
    return passed == required ? 0 : 1;
}
