// stub12.cpp — a stand-in ID3D12Device / ID3D12CommandQueue for native D3D11 mode (XRL_D3D11=native).
//
// The guest's d3d11.dll was written for D3D11On12: it asks the host for a D3D12 device
// (d3d12_create_device), makes a direct command queue on it with a COM call, and passes both to
// d3d11on12_create and to the swapchain call. A native D3D11 device needs neither. Instead of
// changing Wine and the ABI, native mode answers d3d12_create_device with this object: it is a
// COM object with a native vtable like any other host object (the layer hooks it the same way), its
// CreateCommandQueue returns a stand-in queue, and everything else fails (E_NOTIMPL). No D3D12
// device is created on the adapter, so no D3D12 memory is spent. A D3D12 game gets nothing usable
// in this mode; Rocket League is D3D11 only.
#include "common.h"

#include <atomic>

namespace xrl {
namespace {

template <class Base>
class StubObject : public Base {
public:
    virtual ~StubObject() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override {
        if (size) *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }

protected:
    std::atomic<ULONG> refs_{1};
};

class StubQueue final : public StubObject<ID3D12CommandQueue> {
public:
    explicit StubQueue(ID3D12Device* dev) : dev_(dev) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
            *ppv = static_cast<ID3D12CommandQueue*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppv) override { return dev_->QueryInterface(riid, ppv); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
                                              const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                              const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
                                              D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*,
                                            const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT, ID3D12CommandList* const*) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence*, UINT64) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64*, UINT64*) override { return E_NOTIMPL; }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_COMMAND_QUEUE_DESC desc{};

private:
    ComPtr<ID3D12Device> dev_;   // as a real queue does; the device does not own its queues
};

class StubDevice final : public StubObject<ID3D12Device> {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device)) {
            *ppv = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        auto* q = new (std::nothrow) StubQueue(this);
        if (!q) return E_OUTOFMEMORY;
        if (d) q->desc = *d;
        HRESULT hr = q->QueryInterface(riid, ppv);
        q->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                ID3D12PipelineState*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE, void*, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override { return 0; }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void*, SIZE_T, REFIID, void**) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override {}
    void STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {}
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE) override {}
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT, const D3D12_RESOURCE_DESC*) override {
        return {};
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override { return {}; }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                      D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC*, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                   const D3D12_CLEAR_VALUE*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
                                                     REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC*, UINT, UINT, UINT64,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT*, UINT*, UINT64*, UINT64*) override {}
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID,
                                                     void**) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*, UINT,
                                             D3D12_SUBRESOURCE_TILING*) override {}
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return {}; }
};

}  // namespace

HRESULT CreateStubD3D12Device(ComPtr<ID3D12Device>& out) {
    auto* d = new (std::nothrow) StubDevice();
    if (!d) return E_OUTOFMEMORY;
    out.Attach(d);
    return S_OK;
}

}  // namespace xrl
