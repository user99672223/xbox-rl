// selftest.cpp — end-to-end exercise of the real ops table + the one-call probe.
//
// Drives the library the way the layer/Wine side will: ops->d3d12_create_device,
// then NATIVE vtable calls on the returned handles (CreateCommandQueue,
// CreateCommittedResource, ... — what the layer's thunks will do on the guest's
// behalf; a handle is the raw COM pointer), then ops->create_swapchain /
// get_buffer / present / d3d11on12_create / release. Each drawn frame is verified
// by GPU readback so "it presented" means "the pixels we cleared are what the
// swapchain buffer holds".
#include "common.h"

#include <cstdio>
#include <cstring>

namespace xrl {
namespace {

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = a;
    bar.Transition.StateAfter = b;
    return bar;
}

// Everything needed to clear + read back swapchain buffers via D3D12.
struct Rig {
    const xrl_gfx_ops* ops = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    xrl_handle hsc = XRL_INVALID_HANDLE;
    UINT count = 0;

    std::vector<ComPtr<ID3D12Resource>> buf;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_inc = 0;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 rb_size = 0;
    UINT width = 0, height = 0;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_val = 0;
    HANDLE ev = nullptr;

    ~Rig() {
        if (ev) CloseHandle(ev);
    }

    HRESULT Init() {
        for (UINT i = 0; i < count; ++i) {
            xrl_handle h = 0;
            if (ops->swapchain_get_buffer(hsc, i, &h) != XRL_OK) return E_FAIL;
            buf.emplace_back(FromHandle<ID3D12Resource>(h));  // our own reference ...
            ops->release(h);                                  // ... so drop the handle's
        }
        D3D12_RESOURCE_DESC bd = buf[0]->GetDesc();
        width = (UINT)bd.Width;
        height = bd.Height;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = count;
        HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        if (FAILED(hr)) return hr;
        rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < count; ++i) {
            dev->CreateRenderTargetView(buf[i].Get(), nullptr, h);
            h.ptr += rtv_inc;
        }

        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (FAILED(hr)) return hr;
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr)) return hr;
        list->Close();

        UINT rows = 0;
        UINT64 row_size = 0;
        dev->GetCopyableFootprints(&bd, 0, 1, 0, &fp, &rows, &row_size, &rb_size);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = rb_size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return hr;

        hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return hr;
        ev = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        return ev ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT Wait() {
        HRESULT hr = q->Signal(fence.Get(), ++fence_val);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(fence_val, ev);
        if (FAILED(hr)) return hr;
        if (WaitForSingleObjectEx(ev, 10000, FALSE) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return dev->GetDeviceRemovedReason();
    }

    void RecordCopyToReadback(UINT idx) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = buf[idx].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    HRESULT Submit() {
        HRESULT hr = list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* l[1] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        return S_OK;
    }

    // Clear buffer `idx` via D3D12 (PRESENT -> RT -> PRESENT), optionally copying
    // it to the readback buffer before returning it to PRESENT.
    HRESULT ClearD3D12(UINT idx, const float c[4], bool with_readback) {
        HRESULT hr = alloc->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(alloc.Get(), nullptr);
        if (FAILED(hr)) return hr;
        D3D12_RESOURCE_BARRIER b =
            Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)idx * rtv_inc;
        list->ClearRenderTargetView(h, c, 0, nullptr);
        if (with_readback) {
            b = Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            RecordCopyToReadback(idx);
            b = Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        } else {
            b = Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        list->ResourceBarrier(1, &b);
        return Submit();
    }

    // Copy a PRESENT-state buffer (already drawn by someone else) to readback.
    HRESULT ReadbackOnly(UINT idx) {
        HRESULT hr = alloc->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(alloc.Get(), nullptr);
        if (FAILED(hr)) return hr;
        D3D12_RESOURCE_BARRIER b =
            Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b);
        RecordCopyToReadback(idx);
        b = Transition(buf[idx].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &b);
        return Submit();
    }

    // Centre pixel as 0xAARRGGBB (BGRA8 little-endian).
    HRESULT CentrePixel(uint32_t* px) {
        D3D12_RANGE rr{0, (SIZE_T)rb_size};
        uint8_t* p = nullptr;
        HRESULT hr = readback->Map(0, &rr, reinterpret_cast<void**>(&p));
        if (FAILED(hr)) return hr;
        const uint8_t* row = p + fp.Offset + (size_t)(height / 2) * fp.Footprint.RowPitch;
        memcpy(px, row + (size_t)(width / 2) * 4, 4);
        D3D12_RANGE wr{0, 0};
        readback->Unmap(0, &wr);
        return S_OK;
    }
};

double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

}  // namespace
}  // namespace xrl

using namespace xrl;

extern "C" XRL_GFX_API xrl_status xrl_gfx_selftest(uint32_t frames, xrl_gfx_selftest_result* out) {
    xrl_gfx_selftest_result local;
    xrl_gfx_selftest_result* r = out ? out : &local;
    memset(r, 0, sizeof *r);
    r->struct_size = sizeof *r;
    r->device = r->d3d11on12 = r->swapchain = r->d3d12_present = r->d3d11on12_present = r->resize =
        XRL_ERR_NOTAVAIL;
    {
        std::lock_guard<std::mutex> l(G().mu);
        if (!G().inited) return XRL_ERR_GENERIC;
    }
    if (!frames) frames = 60;

    const xrl_gfx_ops* ops = xrl_gfx_get_ops();
    const bool have_window = GetCoreWindow() != nullptr || GetTestHwnd() != nullptr;
    Log(LOG_INFO, "selftest: begin frames=%u have_window=%d on tid %lu", frames, (int)have_window,
        (unsigned long)GetCurrentThreadId());

    // ---- 1. device + caps (handle IS the ID3D12Device*)
    xrl_handle hdev = 0;
    r->device = ops->d3d12_create_device(&hdev);
    Log(LOG_INFO, "selftest: d3d12_create_device -> %d", r->device);
    if (r->device != XRL_OK) return r->device;
    ComPtr<ID3D12Device> dev = FromHandle<ID3D12Device>(hdev);
    xrl_gfx_report_caps(hdev, nullptr);

    // The guest's own present queue, created the way the layer's thunk would.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q;
    HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        r->device = Fail(hr, "selftest CreateCommandQueue");
        ops->release(hdev);
        return r->device;
    }
    const xrl_handle hq = (xrl_handle)(uintptr_t)q.Get();  // borrowed: ops never takes ownership of an input

    // ---- 2. D3D11On12 over the same device + queue
    xrl_handle h11 = 0, hc11 = 0;
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11On12Device> on12;
    if (ops->d3d11on12_create) {
        r->d3d11on12 = ops->d3d11on12_create(hdev, hq, &h11, &hc11);
        if (r->d3d11on12 == XRL_OK) {
            dev11 = FromHandle<ID3D11Device>(h11);
            ctx11 = FromHandle<ID3D11DeviceContext>(hc11);
            dev11.As(&on12);
        }
    } else {
        Log(LOG_WARN, "selftest: ops->d3d11on12_create is NULL (preflight judged D3D11On12 non-viable)");
    }
    Log(LOG_INFO, "selftest: d3d11on12_create -> %d", r->d3d11on12);

    // ---- 3. swapchain + present phases
    xrl_handle hsc = 0;
    if (have_window) {
        r->swapchain = ops->create_swapchain_for_corewindow(hdev, hq, 0, 0, 3, DXGI_FORMAT_B8G8R8A8_UNORM, &hsc);
        Log(LOG_INFO, "selftest: create_swapchain_for_corewindow -> %d", r->swapchain);
    }
    if (r->swapchain == XRL_OK) {
        Rig rig;
        rig.ops = ops;
        rig.dev = dev.Get();
        rig.q = q.Get();
        rig.hsc = hsc;
        rig.count = 3;
        hr = rig.Init();
        if (FAILED(hr)) {
            Fail(hr, "selftest rig init");
            r->d3d12_present = HrToStatus(hr);
        } else {
            // Phase A: D3D12 clears, vsync'd presents, readback on the last frame.
            double t0 = NowMs();
            xrl_status st = XRL_OK;
            for (uint32_t f = 0; f < frames && st == XRL_OK; ++f) {
                uint32_t idx = 0;
                xrl_gfx_swapchain_current_index(hsc, &idx);
                const bool last = (f == frames - 1);
                const float c[4] = {last ? 1.f : (float)(f % 16) / 16.f, 0.f, 0.f, 1.f};
                hr = rig.ClearD3D12(idx, c, last);
                if (SUCCEEDED(hr)) st = ops->swapchain_present(hsc, 1, 0);
                if (SUCCEEDED(hr) && st == XRL_OK) hr = rig.Wait();
                if (FAILED(hr)) st = HrToStatus(hr);
            }
            r->avg_present_ms = (NowMs() - t0) / frames;
            if (st == XRL_OK) {
                hr = rig.CentrePixel(&r->d3d12_pixel);
                if (FAILED(hr) || r->d3d12_pixel != 0xFFFF0000u) {
                    Log(LOG_ERROR, "selftest: D3D12 readback pixel=0x%08X expected 0xFFFF0000 (hr=0x%08lX)",
                        r->d3d12_pixel, (unsigned long)hr);
                    st = XRL_ERR_GENERIC;
                }
            }
            r->d3d12_present = st;
            Log(LOG_INFO, "selftest: D3D12 phase -> %d, %u frames, avg %.2f ms/frame (vsync=1), pixel=0x%08X", st,
                frames, r->avg_present_ms, r->d3d12_pixel);

            // Phase B: clear through a D3D11On12-wrapped backbuffer (the RL d3d11 path).
            if (r->d3d11on12 == XRL_OK && r->d3d12_present == XRL_OK && on12) {
                std::vector<ComPtr<ID3D11Resource>> w11(rig.count);
                std::vector<ComPtr<ID3D11RenderTargetView>> rtv11(rig.count);
                hr = S_OK;
                for (UINT i = 0; i < rig.count && SUCCEEDED(hr); ++i) {
                    D3D11_RESOURCE_FLAGS rf{};
                    rf.BindFlags = D3D11_BIND_RENDER_TARGET;
                    hr = on12->CreateWrappedResource(rig.buf[i].Get(), &rf, D3D12_RESOURCE_STATE_PRESENT,
                                                     D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&w11[i]));
                    if (SUCCEEDED(hr)) hr = dev11->CreateRenderTargetView(w11[i].Get(), nullptr, &rtv11[i]);
                }
                st = SUCCEEDED(hr) ? XRL_OK : HrToStatus(hr);
                const uint32_t fb = 6;
                for (uint32_t f = 0; f < fb && st == XRL_OK; ++f) {
                    uint32_t idx = 0;
                    xrl_gfx_swapchain_current_index(hsc, &idx);
                    const bool last = (f == fb - 1);
                    const float green[4] = {0.f, last ? 1.f : 0.5f, 0.f, 1.f};
                    ID3D11Resource* arr[1] = {w11[idx].Get()};
                    on12->AcquireWrappedResources(arr, 1);
                    ctx11->ClearRenderTargetView(rtv11[idx].Get(), green);
                    on12->ReleaseWrappedResources(arr, 1);
                    ctx11->Flush();
                    if (last) hr = rig.ReadbackOnly(idx);
                    if (SUCCEEDED(hr)) st = ops->swapchain_present(hsc, 1, 0);
                    if (SUCCEEDED(hr) && st == XRL_OK) hr = rig.Wait();
                    if (FAILED(hr)) st = HrToStatus(hr);
                }
                if (st == XRL_OK) {
                    hr = rig.CentrePixel(&r->d3d11_pixel);
                    if (FAILED(hr) || r->d3d11_pixel != 0xFF00FF00u) {
                        Log(LOG_ERROR, "selftest: D3D11On12 readback pixel=0x%08X expected 0xFF00FF00 (hr=0x%08lX)",
                            r->d3d11_pixel, (unsigned long)hr);
                        st = XRL_ERR_GENERIC;
                    }
                }
                r->d3d11on12_present = st;
                Log(LOG_INFO, "selftest: D3D11On12 phase -> %d, pixel=0x%08X", st, r->d3d11_pixel);
                // Drop every d3d11 reference to the d3d12 buffers.
                ctx11->ClearState();
                ctx11->Flush();
                rtv11.clear();
                w11.clear();
            }

            // Phase C: ResizeBuffers (RL changes display mode) — requires zero buffer refs.
            rig.Wait();
            const UINT old_w = rig.width, old_h = rig.height;
            rig.buf.clear();
            rig.readback.Reset();
            xrl_status rs = xrl_gfx_swapchain_resize(hsc, 0, old_w / 2, old_h / 2, 0);
            if (rs == XRL_OK) {
                xrl_handle hb = 0;
                rs = ops->swapchain_get_buffer(hsc, 0, &hb);
                if (rs == XRL_OK) {
                    ComPtr<ID3D12Resource> b = FromHandle<ID3D12Resource>(hb);
                    ops->release(hb);
                    D3D12_RESOURCE_DESC d = b->GetDesc();
                    if (d.Width != old_w / 2 || d.Height != old_h / 2) {
                        Log(LOG_ERROR, "selftest: resize: buffer is %llux%u, expected %ux%u",
                            (unsigned long long)d.Width, d.Height, old_w / 2, old_h / 2);
                        rs = XRL_ERR_GENERIC;
                    }
                }
            }
            r->resize = rs;
            Log(LOG_INFO, "selftest: resize %ux%u -> %ux%u: %d", old_w, old_h, old_w / 2, old_h / 2, rs);
        }
    }  // Rig dtor

    if (hsc) ops->release(hsc);
    if (hc11) ops->release(hc11);
    if (h11) ops->release(h11);
    on12.Reset();
    ctx11.Reset();
    dev11.Reset();
    q.Reset();
    dev.Reset();
    ops->release(hdev);

    r->pass = (r->device == XRL_OK && r->d3d11on12 == XRL_OK &&
               (!have_window || (r->swapchain == XRL_OK && r->d3d12_present == XRL_OK &&
                                 r->d3d11on12_present == XRL_OK && r->resize == XRL_OK)))
                  ? 1
                  : 0;
    Log(r->pass ? LOG_INFO : LOG_ERROR,
        "selftest: %s (device=%d d3d11on12=%d swapchain=%d d3d12_present=%d d3d11on12_present=%d resize=%d)",
        r->pass ? "PASS" : "FAIL", r->device, r->d3d11on12, r->swapchain, r->d3d12_present, r->d3d11on12_present,
        r->resize);
    return r->pass ? XRL_OK : XRL_ERR_GENERIC;
}

extern "C" XRL_GFX_API int xrl_gfx_run_probe(const xrl_gfx_config* cfg) {
    bool did_init = false;
    bool inited;
    {
        std::lock_guard<std::mutex> l(G().mu);
        inited = G().inited;
    }
    if (!inited) {
        if (!cfg) return 1;
        xrl_status s = xrl_gfx_init(cfg);
        if (s != XRL_OK) return 1;
        did_init = true;
    }
    Log(LOG_INFO, "=== xrl_gfx probe BEGIN (caller tid %lu) ===", (unsigned long)GetCurrentThreadId());

    xrl_gfx_caps caps{};
    xrl_status s_caps = xrl_gfx_report_caps(XRL_INVALID_HANDLE, &caps);

    xrl_gfx_d3d11_decision dec{};
    xrl_gfx_preflight(&dec);
    Log(LOG_INFO, "probe: d3d11 backend=%s reason=%u hr=0x%08lX \"%s\"",
        dec.backend == XRL_GFX_D3D11_ON12 ? "D3D11On12" : "DXVK_DOZEN_NEEDED_OR_UNDECIDED", dec.reason,
        (unsigned long)dec.hresult, dec.detail);

    xrl_gfx_selftest_result res{};
    xrl_gfx_selftest(60, &res);

    const bool pass = s_caps == XRL_OK && dec.backend == XRL_GFX_D3D11_ON12 && res.pass;
    Log(pass ? LOG_INFO : LOG_ERROR,
        "=== xrl_gfx probe RESULT: %s | adapter=\"%s\" vendor=0x%04X device=0x%04X FL=0x%X bind_tier=%u heap_tier=%u "
        "d3d11on12=%s present_avg=%.2fms swapchain_stages=%s ===",
        pass ? "PASS" : "FAIL", caps.adapter_name, caps.vendor_id, caps.device_id, caps.max_feature_level,
        caps.resource_binding_tier, caps.resource_heap_tier, dec.backend == XRL_GFX_D3D11_ON12 ? "OK" : "FAILED",
        res.avg_present_ms, res.swapchain == XRL_ERR_NOTAVAIL ? "SKIPPED(no window)" : "run");

    if (did_init) xrl_gfx_shutdown();
    return pass ? 0 : 1;
}
