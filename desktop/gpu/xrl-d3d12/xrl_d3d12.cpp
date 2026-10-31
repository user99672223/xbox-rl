/*
 * xrl_d3d12.cpp - libd3d12.so and libdxcore.so for Linux programs on the xbox-rl layer.
 *
 * Mesa's d3d12 gallium driver on Linux (the WSL build) loads libd3d12.so and libdxcore.so and calls them, and every
 * D3D12 object they hand out, through Win64 COM. On WSL those are Microsoft's Linux builds of the runtime. Here the
 * runtime is the console's own D3D12 (a Windows DLL in the host app): Mesa is built with STDMETHODCALLTYPE/WINAPI =
 * __attribute__((ms_abi)) (desktop/gpu/build-mesa.sh), so it calls the native objects directly, and the layer makes
 * those calls safe from guest threads (layer/src/com.c: host TEB view + host stack per COM call).
 *
 * This library is the small part of the runtime that is not a method of a native object:
 *   D3D12CreateDevice        the device comes from the host's op table (XRL_SYS_GET_GFX_OPS ->
 *                            xrl_gfx_ops.d3d12_create_device), created once and shared; adapter arguments are ignored
 *                            (the console has one GPU; the CI test host gives WARP)
 *   D3D12SerializeVersionedRootSignature / D3D12SerializeRootSignature
 *                            written here (RTS0 in a DXBC container, signed like DXIL: xrl_dxil_hash.c)
 *   D3D12EnableExperimentalFeatures   accepted and ignored: Mesa's Linux path asks for experimental shader models
 *                            (unsigned DXIL); ours signs its DXIL instead (patches/mesa), which a stock runtime accepts
 *   D3D12GetDebugInterface   E_NOINTERFACE (no SDK layers on the console)
 *   DXCoreCreateAdapterFactory   one adapter, described by xrl_gfx_ops.get_adapter_info
 * Everything it exports is ms_abi (Mesa calls these through WINAPI function pointers); the COM objects it makes
 * itself (blobs, the DXCore factory/list/adapter) are C++ classes with ms_abi virtual methods, the same vtable
 * layout MSVC gives them.
 *
 * XRL_D3D12_DEBUG=1 logs every call to stderr.
 */
#define INITGUID   /* define (not just declare) the IIDs and DXCore attribute GUIDs used below */
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "xrl_abi.h"
#include "xrl_dxil_hash.h"

#ifndef XRL_WIN64_ABI
#error "build with -DXRL_WIN64_ABI (desktop/gpu/build-mesa.sh): the console's D3D12 is Win64 COM"
#endif

#define EXPORT extern "C" __attribute__((visibility("default")))

static int g_debug = -1;
static void dbg(const char *fmt, ...)
{
    if (g_debug < 0) { const char *e = getenv("XRL_D3D12_DEBUG"); g_debug = e && *e && *e != '0'; }
    if (!g_debug) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "xrl-d3d12: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
static void err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "xrl-d3d12: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ the host: op table, device, adapter */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const xrl_gfx_ops *g_ops;
static bool g_ops_tried;
static ID3D12Device *g_dev;
static xrl_adapter_info g_info;
static bool g_have_info;

#define OPS_HAS(o, f) ((o)->struct_size >= offsetof(xrl_gfx_ops, f) + sizeof(void *) && (o)->f)

static const xrl_gfx_ops *host_ops(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_ops_tried) {
        g_ops_tried = true;
        long r = syscall(XRL_SYS_GET_GFX_OPS, 0, 0, 0, 0, 0, 0);
        if (r == -1 || r == 0) err("no host graphics op table (not on the xbox-rl layer?)");
        else g_ops = (const xrl_gfx_ops *)r;
        if (g_ops) {
            dbg("host gfx ops %p, struct_size %u", (void *)g_ops, g_ops->struct_size);
            if (OPS_HAS(g_ops, get_adapter_info)) {
                memset(&g_info, 0, sizeof g_info);
                g_info.struct_size = sizeof g_info;
                g_have_info = g_ops->get_adapter_info(&g_info) == XRL_OK;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return g_ops;
}

/* the host's device, created on first use and kept (the console has one GPU; every screen shares it) */
static ID3D12Device *host_device(void)
{
    const xrl_gfx_ops *ops = host_ops();
    if (!ops || !OPS_HAS(ops, d3d12_create_device)) return NULL;
    /* a real host table describes its adapter; the test host's stub table (no XRL_REAL_GFX) hands out a fake
     * device handle and has no get_adapter_info: no GPU then */
    if (!g_have_info) {
        static bool said;
        if (!said) { said = true; err("host op table has no adapter info (a stub table?): no GPU"); }
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_dev) {
        xrl_handle h = 0;
        xrl_status st = ops->d3d12_create_device(&h);
        if (st != XRL_OK || !h) {
            int hr = OPS_HAS(ops, last_hresult) ? ops->last_hresult() : 0;
            err("host d3d12_create_device failed: status %d, hr 0x%08x", st, (unsigned)hr);
        } else {
            g_dev = (ID3D12Device *)(uintptr_t)h;
            dbg("host device %p", (void *)g_dev);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return g_dev;
}

static bool guid_eq(REFGUID a, REFGUID b) { return !memcmp(&a, &b, sizeof(GUID)); }

/* ------------------------------------------------------------------ ID3DBlob */
class Blob final : public ID3DBlob {
    LONG refs = 1;
public:
    void *data; SIZE_T size;
    Blob(void *d, SIZE_T n) : data(d), size(n) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (guid_eq(riid, IID_IUnknown) || guid_eq(riid, IID_ID3D10Blob)) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return __atomic_add_fetch(&refs, 1, __ATOMIC_SEQ_CST); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = __atomic_sub_fetch(&refs, 1, __ATOMIC_SEQ_CST);
        if (!r) { free(data); delete this; }
        return r;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data; }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return size; }
};

static Blob *text_blob(const char *msg)
{
    size_t n = strlen(msg) + 1;
    char *p = (char *)malloc(n);
    if (!p) return NULL;
    memcpy(p, msg, n);
    return new Blob(p, n);
}

/* ------------------------------------------------------------------ root signature serializer */
/* RTS0 (the root signature part of a DXBC container), all little-endian u32:
 *   header   version (1 = 1.0, 2 = 1.1, 3 = 1.2), parameter count, parameters offset, static sampler count,
 *            static samplers offset, flags
 *   parameters: {type, visibility, payload offset} each, then the payloads:
 *     descriptor table   {range count, ranges offset}, then the ranges: 1.0 {type, count, base register, space,
 *                        offset in table}, 1.1+ {type, count, base register, space, flags, offset in table}
 *     32-bit constants   {register, space, count}
 *     CBV/SRV/UAV        1.0 {register, space}, 1.1+ {register, space, flags}
 *   static samplers: D3D12_STATIC_SAMPLER_DESC as 13 u32 (1.2: D3D12_STATIC_SAMPLER_DESC1, 14 with its flags)
 * Offsets count from the start of the part. Wrapped in a one-part DXBC container signed with the retail hash. */
struct wbuf { uint8_t *p; size_t n, cap; bool oom; };
static size_t w_put(wbuf *b, const void *d, size_t n)
{
    size_t at = b->n;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1024;
        while (c < b->n + n) c *= 2;
        uint8_t *q = (uint8_t *)realloc(b->p, c);
        if (!q) { b->oom = true; return at; }
        b->p = q; b->cap = c;
    }
    if (d) memcpy(b->p + b->n, d, n); else memset(b->p + b->n, 0, n);
    b->n += n;
    return at;
}
static size_t w_u32(wbuf *b, uint32_t v) { return w_put(b, &v, 4); }
static void w_set(wbuf *b, size_t at, uint32_t v) { if (!b->oom) memcpy(b->p + at, &v, 4); }

static HRESULT serialize_rts0(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *d, wbuf *b, const char **why)
{
    uint32_t ver = d->Version, np, ns, flags;
    if (ver == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        np = d->Desc_1_0.NumParameters; ns = d->Desc_1_0.NumStaticSamplers; flags = d->Desc_1_0.Flags;
    } else if (ver == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        np = d->Desc_1_1.NumParameters; ns = d->Desc_1_1.NumStaticSamplers; flags = d->Desc_1_1.Flags;
    } else if (ver == D3D_ROOT_SIGNATURE_VERSION_1_2) {
        np = d->Desc_1_2.NumParameters; ns = d->Desc_1_2.NumStaticSamplers; flags = d->Desc_1_2.Flags;
    } else { *why = "unknown root signature version"; return E_INVALIDARG; }

    w_u32(b, ver); w_u32(b, np);
    size_t at_poff = w_u32(b, 0);
    w_u32(b, ns);
    size_t at_soff = w_u32(b, 0);
    w_u32(b, flags);
    w_set(b, at_poff, (uint32_t)b->n);
    size_t at_params = b->n;
    w_put(b, NULL, 12u * np);
    for (uint32_t i = 0; i < np; i++) {
        /* 1.1 and 1.2 share D3D12_ROOT_PARAMETER1 */
        const D3D12_ROOT_PARAMETER *p0 = ver == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &d->Desc_1_0.pParameters[i] : NULL;
        const D3D12_ROOT_PARAMETER1 *p1 = ver == D3D_ROOT_SIGNATURE_VERSION_1_1 ? &d->Desc_1_1.pParameters[i] :
                                          ver == D3D_ROOT_SIGNATURE_VERSION_1_2 ? &d->Desc_1_2.pParameters[i] : NULL;
        uint32_t type = p0 ? p0->ParameterType : p1->ParameterType;
        uint32_t vis = p0 ? p0->ShaderVisibility : p1->ShaderVisibility;
        size_t at = at_params + 12u * i;
        w_set(b, at, type); w_set(b, at + 4, vis); w_set(b, at + 8, (uint32_t)b->n);
        switch (type) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            uint32_t nr = p0 ? p0->DescriptorTable.NumDescriptorRanges : p1->DescriptorTable.NumDescriptorRanges;
            w_u32(b, nr);
            size_t at_roff = w_u32(b, 0);
            w_set(b, at_roff, (uint32_t)b->n);
            for (uint32_t r = 0; r < nr; r++) {
                if (p0) {
                    const D3D12_DESCRIPTOR_RANGE *g = &p0->DescriptorTable.pDescriptorRanges[r];
                    w_u32(b, g->RangeType); w_u32(b, g->NumDescriptors); w_u32(b, g->BaseShaderRegister);
                    w_u32(b, g->RegisterSpace); w_u32(b, g->OffsetInDescriptorsFromTableStart);
                } else {
                    const D3D12_DESCRIPTOR_RANGE1 *g = &p1->DescriptorTable.pDescriptorRanges[r];
                    w_u32(b, g->RangeType); w_u32(b, g->NumDescriptors); w_u32(b, g->BaseShaderRegister);
                    w_u32(b, g->RegisterSpace); w_u32(b, g->Flags); w_u32(b, g->OffsetInDescriptorsFromTableStart);
                }
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            const D3D12_ROOT_CONSTANTS *c = p0 ? &p0->Constants : &p1->Constants;
            w_u32(b, c->ShaderRegister); w_u32(b, c->RegisterSpace); w_u32(b, c->Num32BitValues);
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_CBV: case D3D12_ROOT_PARAMETER_TYPE_SRV: case D3D12_ROOT_PARAMETER_TYPE_UAV:
            if (p0) { w_u32(b, p0->Descriptor.ShaderRegister); w_u32(b, p0->Descriptor.RegisterSpace); }
            else { w_u32(b, p1->Descriptor.ShaderRegister); w_u32(b, p1->Descriptor.RegisterSpace); w_u32(b, p1->Descriptor.Flags); }
            break;
        default:
            *why = "unknown root parameter type";
            return E_INVALIDARG;
        }
    }
    w_set(b, at_soff, (uint32_t)b->n);
    for (uint32_t i = 0; i < ns; i++) {
        if (ver == D3D_ROOT_SIGNATURE_VERSION_1_2) {
            static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC1) == 14 * 4, "sampler layout");
            w_put(b, &d->Desc_1_2.pStaticSamplers[i], sizeof(D3D12_STATIC_SAMPLER_DESC1));
        } else {
            static_assert(sizeof(D3D12_STATIC_SAMPLER_DESC) == 13 * 4, "sampler layout");
            w_put(b, ver == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &d->Desc_1_0.pStaticSamplers[i] : &d->Desc_1_1.pStaticSamplers[i],
                  sizeof(D3D12_STATIC_SAMPLER_DESC));
        }
    }
    return b->oom ? E_OUTOFMEMORY : S_OK;
}

/* exported for desktop/gpu/tests (the same bytes D3D12SerializeVersionedRootSignature returns) */
EXPORT HRESULT xrl_d3d12_serialize_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, void **out,
                                                  size_t *out_size, const char **why)
{
    wbuf part = {};
    *why = "out of memory";
    HRESULT hr = serialize_rts0(desc, &part, why);
    if (FAILED(hr)) { free(part.p); return hr; }
    wbuf c = {};
    w_put(&c, "DXBC", 4); w_put(&c, NULL, 16);
    uint16_t ver[2] = { 1, 0 };
    w_put(&c, ver, 4);
    w_u32(&c, (uint32_t)(32 + 4 + 8 + part.n));
    w_u32(&c, 1);
    w_u32(&c, 36);
    w_put(&c, "RTS0", 4); w_u32(&c, (uint32_t)part.n);
    w_put(&c, part.p, part.n);
    free(part.p);
    if (c.oom) { free(c.p); return E_OUTOFMEMORY; }
    xrl_dxil_container_sign(c.p, (uint32_t)c.n);
    *out = c.p; *out_size = c.n;
    return S_OK;
}

EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
                                                           ID3DBlob **blob, ID3DBlob **error)
{
    if (error) *error = NULL;
    if (!desc || !blob) return E_INVALIDARG;
    *blob = NULL;
    void *p; size_t n; const char *why = "";
    HRESULT hr = xrl_d3d12_serialize_root_signature(desc, &p, &n, &why);
    dbg("D3D12SerializeVersionedRootSignature(version %u) -> 0x%08x, %zu bytes", desc->Version, (unsigned)hr,
        SUCCEEDED(hr) ? n : 0);
    if (FAILED(hr)) {
        if (error) *error = text_blob(why);
        return hr;
    }
    Blob *b = new Blob(p, n);
    *blob = b;
    return S_OK;
}

EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *desc,
                                                  D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error)
{
    if (!desc || version != D3D_ROOT_SIGNATURE_VERSION_1_0) {
        if (error) *error = text_blob("D3D12SerializeRootSignature: only version 1.0");
        return E_INVALIDARG;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    v.Desc_1_0 = *desc;
    return D3D12SerializeVersionedRootSignature(&v, blob, error);
}

/* ------------------------------------------------------------------ device and runtime entry points */
EXPORT HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL min_level, REFIID riid, void **out)
{
    (void)adapter;
    ID3D12Device *dev = host_device();
    dbg("D3D12CreateDevice(adapter %p, level 0x%x) -> host device %p", (void *)adapter, (unsigned)min_level, (void *)dev);
    if (!dev) return E_FAIL;
    if (!out) return S_FALSE;   /* "would succeed" */
    return dev->QueryInterface(riid, out);
}

EXPORT HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **out)
{
    (void)riid;
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

EXPORT HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT n, const IID *iids, void *structs, UINT *sizes)
{
    (void)iids; (void)structs; (void)sizes;
    dbg("D3D12EnableExperimentalFeatures(%u): ignored (DXIL is signed instead)", n);
    return S_OK;
}

/* ------------------------------------------------------------------ DXCore: one adapter */
static void adapter_description(char *out, size_t n)
{
    host_ops();
    size_t j = 0;
    if (g_have_info)
        for (size_t i = 0; i < 128 && g_info.description[i] && j + 4 < n; i++) {
            uint32_t c = g_info.description[i];   /* UTF-16 -> UTF-8 (BMP; a surrogate becomes '?') */
            if (c < 0x80) out[j++] = (char)c;
            else if (c < 0x800) { out[j++] = (char)(0xc0 | c >> 6); out[j++] = (char)(0x80 | (c & 0x3f)); }
            else if (c >= 0xd800 && c < 0xe000) out[j++] = '?';
            else { out[j++] = (char)(0xe0 | c >> 12); out[j++] = (char)(0x80 | (c >> 6 & 0x3f)); out[j++] = (char)(0x80 | (c & 0x3f)); }
        }
    if (!j) j = (size_t)snprintf(out, n, "xbox-rl host GPU");
    out[j < n ? j : n - 1] = 0;
}

static bool is_warp(void) { host_ops(); return g_have_info && g_info.vendor_id == 0x1414 && g_info.device_id == 0x8c; }

class Factory;
class Adapter final : public IDXCoreAdapter {
    LONG refs = 1;
public:
    IDXCoreAdapterFactory *factory;
    explicit Adapter(IDXCoreAdapterFactory *f) : factory(f) { factory->AddRef(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (guid_eq(riid, IID_IUnknown) || guid_eq(riid, IID_IDXCoreAdapter)) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return __atomic_add_fetch(&refs, 1, __ATOMIC_SEQ_CST); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = __atomic_sub_fetch(&refs, 1, __ATOMIC_SEQ_CST);
        if (!r) { factory->Release(); delete this; }
        return r;
    }
    bool STDMETHODCALLTYPE IsValid() override { return true; }
    bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID a) override
    {
        return guid_eq(a, DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS) || guid_eq(a, DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE);
    }
    bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty p) override
    {
        size_t n;
        return SUCCEEDED(GetPropertySize(p, &n));
    }
    HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty p, size_t *n) override
    {
        switch (p) {
        case DXCoreAdapterProperty::InstanceLuid: *n = sizeof(LUID); return S_OK;
        case DXCoreAdapterProperty::DriverVersion: *n = sizeof(uint64_t); return S_OK;
        case DXCoreAdapterProperty::DriverDescription: { char d[512]; adapter_description(d, sizeof d); *n = strlen(d) + 1; return S_OK; }
        case DXCoreAdapterProperty::HardwareID: *n = sizeof(DXCoreHardwareID); return S_OK;
        case DXCoreAdapterProperty::HardwareIDParts: *n = sizeof(DXCoreHardwareIDParts); return S_OK;
        case DXCoreAdapterProperty::DedicatedAdapterMemory: case DXCoreAdapterProperty::DedicatedSystemMemory:
        case DXCoreAdapterProperty::SharedSystemMemory: *n = sizeof(uint64_t); return S_OK;
        case DXCoreAdapterProperty::IsHardware: case DXCoreAdapterProperty::IsIntegrated:
        case DXCoreAdapterProperty::IsDetachable: *n = sizeof(bool); return S_OK;
        default: *n = 0; return DXGI_ERROR_INVALID_CALL;   /* "not supported" */
        }
    }
    HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty p, size_t size, void *buf) override
    {
        size_t need;
        HRESULT hr = GetPropertySize(p, &need);
        if (FAILED(hr)) return hr;
        if (!buf) return E_POINTER;
        if (size < need) return E_INVALIDARG;
        host_ops();
        switch (p) {
        case DXCoreAdapterProperty::InstanceLuid: {
            LUID l = { (uint32_t)g_info.adapter_luid, (int32_t)(g_info.adapter_luid >> 32) };
            memcpy(buf, &l, sizeof l); break;
        }
        case DXCoreAdapterProperty::DriverVersion: { uint64_t v = 0; memcpy(buf, &v, 8); break; }
        case DXCoreAdapterProperty::DriverDescription: adapter_description((char *)buf, size); break;
        case DXCoreAdapterProperty::HardwareID: {
            DXCoreHardwareID h = { g_info.vendor_id, g_info.device_id, g_info.subsys_id, g_info.revision };
            memcpy(buf, &h, sizeof h); break;
        }
        case DXCoreAdapterProperty::HardwareIDParts: {
            DXCoreHardwareIDParts h = { g_info.vendor_id, g_info.device_id, g_info.subsys_id & 0xffff,
                                        g_info.subsys_id >> 16, g_info.revision };
            memcpy(buf, &h, sizeof h); break;
        }
        case DXCoreAdapterProperty::DedicatedAdapterMemory: memcpy(buf, &g_info.dedicated_video_memory, 8); break;
        case DXCoreAdapterProperty::DedicatedSystemMemory: memcpy(buf, &g_info.dedicated_system_memory, 8); break;
        case DXCoreAdapterProperty::SharedSystemMemory: memcpy(buf, &g_info.shared_system_memory, 8); break;
        case DXCoreAdapterProperty::IsHardware: *(bool *)buf = !is_warp(); break;
        case DXCoreAdapterProperty::IsIntegrated: *(bool *)buf = true; break;   /* the console's APU (and WARP) */
        case DXCoreAdapterProperty::IsDetachable: *(bool *)buf = false; break;
        default: return DXGI_ERROR_INVALID_CALL;
        }
        return S_OK;
    }
    bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState s) override
    {
        return s == DXCoreAdapterState::AdapterMemoryBudget;
    }
    HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState s, size_t in_size, const void *in, size_t out_size,
                                         void *out) override
    {
        if (s != DXCoreAdapterState::AdapterMemoryBudget) return DXGI_ERROR_INVALID_CALL;
        if (in_size < sizeof(DXCoreAdapterMemoryBudgetNodeSegmentGroup) || !in ||
            out_size < sizeof(DXCoreAdapterMemoryBudget) || !out)
            return E_INVALIDARG;
        /* No budget query on the host side yet: report the adapter's sizes as the budget, nothing used. Mesa uses
         * this for GL_NVX_gpu_memory_info-style queries and its residency heuristics. */
        host_ops();
        const DXCoreAdapterMemoryBudgetNodeSegmentGroup *g = (const DXCoreAdapterMemoryBudgetNodeSegmentGroup *)in;
        DXCoreAdapterMemoryBudget b = {};
        b.budget = g->segmentGroup == DXCoreSegmentGroup::Local ? g_info.dedicated_video_memory
                                                                 : g_info.shared_system_memory;
        if (!b.budget) b.budget = 1ull << 30;
        b.availableForReservation = b.budget / 2;
        memcpy(out, &b, sizeof b);
        return S_OK;
    }
    bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState) override { return false; }
    HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState, size_t, const void *, size_t, const void *) override
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **out) override { return factory->QueryInterface(riid, out); }
};

class AdapterList final : public IDXCoreAdapterList {
    LONG refs = 1;
    IDXCoreAdapterFactory *factory;
public:
    explicit AdapterList(IDXCoreAdapterFactory *f) : factory(f) { factory->AddRef(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (guid_eq(riid, IID_IUnknown) || guid_eq(riid, IID_IDXCoreAdapterList)) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return __atomic_add_fetch(&refs, 1, __ATOMIC_SEQ_CST); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = __atomic_sub_fetch(&refs, 1, __ATOMIC_SEQ_CST);
        if (!r) { factory->Release(); delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetAdapter(uint32_t i, REFIID riid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = NULL;
        if (i != 0) return E_INVALIDARG;
        Adapter *a = new Adapter(factory);
        HRESULT hr = a->QueryInterface(riid, out);
        a->Release();
        return hr;
    }
    uint32_t STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
    bool STDMETHODCALLTYPE IsStale() override { return false; }
    HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **out) override { return factory->QueryInterface(riid, out); }
    HRESULT STDMETHODCALLTYPE Sort(uint32_t, const DXCoreAdapterPreference *) override { return S_OK; }
    bool STDMETHODCALLTYPE IsAdapterPreferenceSupported(DXCoreAdapterPreference) override { return true; }
};

class Factory final : public IDXCoreAdapterFactory {
    LONG refs = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (guid_eq(riid, IID_IUnknown) || guid_eq(riid, IID_IDXCoreAdapterFactory)) { AddRef(); *out = this; return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return __atomic_add_fetch(&refs, 1, __ATOMIC_SEQ_CST); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = __atomic_sub_fetch(&refs, 1, __ATOMIC_SEQ_CST);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE CreateAdapterList(uint32_t n, const GUID *attrs, REFIID riid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = NULL;
        for (uint32_t i = 0; i < n; i++)
            if (!guid_eq(attrs[i], DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS) &&
                !guid_eq(attrs[i], DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE))
                return DXGI_ERROR_INVALID_CALL;
        if (!host_device()) return E_FAIL;   /* no adapter at all off the layer: Mesa falls back to llvmpipe */
        AdapterList *l = new AdapterList(this);
        HRESULT hr = l->QueryInterface(riid, out);
        l->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterByLuid(const LUID &luid, REFIID riid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = NULL;
        host_ops();
        uint64_t l = (uint32_t)luid.LowPart | ((uint64_t)(uint32_t)luid.HighPart << 32);
        if (!g_have_info || l != g_info.adapter_luid || !host_device()) return E_INVALIDARG;
        Adapter *a = new Adapter(this);
        HRESULT hr = a->QueryInterface(riid, out);
        a->Release();
        return hr;
    }
    bool STDMETHODCALLTYPE IsNotificationTypeSupported(DXCoreNotificationType) override { return false; }
    HRESULT STDMETHODCALLTYPE RegisterEventNotification(IUnknown *, DXCoreNotificationType, PFN_DXCORE_NOTIFICATION_CALLBACK,
                                                        void *, uint32_t *) override
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEventNotification(uint32_t) override { return DXGI_ERROR_INVALID_CALL; }
};

EXPORT HRESULT WINAPI DXCoreCreateAdapterFactory(REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    Factory *f = new Factory();
    HRESULT hr = f->QueryInterface(riid, out);
    f->Release();
    dbg("DXCoreCreateAdapterFactory -> 0x%08x", (unsigned)hr);
    return hr;
}
