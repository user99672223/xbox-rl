// audio_ops.cpp - xrl_audio_ops over native XAudio2 (xaudio2_9.dll on the console;
// decisions.md cat J: mastering voice 48 kHz stereo OK).
//
// Handles are raw pointers (IXAudio2*, IXAudio2MasteringVoice*,
// IXAudio2SourceVoice*). A small registry remembers each handle's kind so that
// release() can DestroyVoice() voices vs Release() the engine.
//
// ABI v1 gaps (see host/ABI-CHANGE-REQUESTS.md #2): `flags` is undefined in the
// header - here it is passed through as XAUDIO2_BUFFER::Flags (only
// XAUDIO2_END_OF_STREAM = 0x40 is honoured); bits_per_sample==32 is taken as
// IEEE float; there is no way to query queued buffers / playback position,
// set volume/pitch, or flush. Buffers are COPIED (the guest owns its memory).
#include "pch.h"
#include "xrl_host.h"

#include <xaudio2.h>
#include <mutex>
#include <unordered_map>

namespace {

enum class Kind { Engine, Master, Source };
struct VoiceCb;
struct Entry { Kind kind; VoiceCb* cb; };

std::mutex g_m;
std::unordered_map<uintptr_t, Entry> g_reg;
IXAudio2MasteringVoice* g_master = nullptr;

struct VoiceCb final : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override { delete[] static_cast<uint8_t*>(ctx); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override {
        XLOGE("audio", "XAudio2 OnVoiceError 0x%08x", (unsigned)hr);
    }
};

xrl_status map_hr(HRESULT hr) {
    if (SUCCEEDED(hr)) return XRL_OK;
    if (hr == XAUDIO2_E_DEVICE_INVALIDATED) return XRL_ERR_DEVICELOST;
    return XRL_ERR_GENERIC;
}

xrl_status op_xaudio2_create(xrl_handle* out) {
    if (!out) return XRL_ERR_GENERIC;
    *out = 0;
    IXAudio2* xa = nullptr;
    HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) { XLOGE("audio", "XAudio2Create failed 0x%08x", (unsigned)hr); return map_hr(hr); }
    { std::lock_guard<std::mutex> lk(g_m); g_reg[(uintptr_t)xa] = {Kind::Engine, nullptr}; }
    *out = (xrl_handle)(uintptr_t)xa;
    return XRL_OK;
}

xrl_status op_create_mastering(xrl_handle xah, uint32_t channels, uint32_t rate, xrl_handle* out) {
    if (!xah || !out) return XRL_ERR_GENERIC;
    *out = 0;
    IXAudio2MasteringVoice* mv = nullptr;
    HRESULT hr = ((IXAudio2*)(uintptr_t)xah)->CreateMasteringVoice(
        &mv, channels ? channels : XAUDIO2_DEFAULT_CHANNELS, rate ? rate : XAUDIO2_DEFAULT_SAMPLERATE);
    if (FAILED(hr)) { XLOGE("audio", "CreateMasteringVoice(%u ch, %u Hz) failed 0x%08x", channels, rate, (unsigned)hr); return map_hr(hr); }
    { std::lock_guard<std::mutex> lk(g_m); g_reg[(uintptr_t)mv] = {Kind::Master, nullptr}; if (!g_master) g_master = mv; }
    *out = (xrl_handle)(uintptr_t)mv;
    return XRL_OK;
}

xrl_status op_create_source(xrl_handle xah, uint32_t channels, uint32_t rate, uint32_t bits, xrl_handle* out) {
    if (!xah || !out || !channels || !rate || !bits) return XRL_ERR_GENERIC;
    *out = 0;
    WAVEFORMATEX f{};
    f.wFormatTag = (bits == 32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels = (WORD)channels;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = (WORD)bits;
    f.nBlockAlign = (WORD)(channels * bits / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    auto* cb = new VoiceCb();
    IXAudio2SourceVoice* sv = nullptr;
    HRESULT hr = ((IXAudio2*)(uintptr_t)xah)->CreateSourceVoice(&sv, &f, 0, XAUDIO2_DEFAULT_FREQ_RATIO, cb);
    if (FAILED(hr)) { delete cb; XLOGE("audio", "CreateSourceVoice(%u ch %u Hz %u bit) failed 0x%08x", channels, rate, bits, (unsigned)hr); return map_hr(hr); }
    { std::lock_guard<std::mutex> lk(g_m); g_reg[(uintptr_t)sv] = {Kind::Source, cb}; }
    *out = (xrl_handle)(uintptr_t)sv;
    return XRL_OK;
}

xrl_status op_submit(xrl_handle vh, const void* data, uint32_t bytes, uint32_t flags) {
    if (!vh || (!data && bytes)) return XRL_ERR_GENERIC;
    auto* copy = new uint8_t[bytes ? bytes : 1];
    if (bytes) memcpy(copy, data, bytes);
    XAUDIO2_BUFFER b{};
    b.Flags = flags & XAUDIO2_END_OF_STREAM;
    b.AudioBytes = bytes;
    b.pAudioData = copy;
    b.pContext = copy;      // freed in OnBufferEnd
    HRESULT hr = ((IXAudio2SourceVoice*)(uintptr_t)vh)->SubmitSourceBuffer(&b);
    if (FAILED(hr)) { delete[] copy; return map_hr(hr); }
    return XRL_OK;
}

xrl_status op_start(xrl_handle vh) { return vh ? map_hr(((IXAudio2SourceVoice*)(uintptr_t)vh)->Start(0)) : XRL_ERR_GENERIC; }
xrl_status op_stop(xrl_handle vh)  { return vh ? map_hr(((IXAudio2SourceVoice*)(uintptr_t)vh)->Stop(0))  : XRL_ERR_GENERIC; }

void op_release(xrl_handle h) {
    if (!h) return;
    Entry e{Kind::Engine, nullptr};
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_reg.find((uintptr_t)h);
        if (it == g_reg.end()) { XLOGW("audio", "release of unknown handle %llx", (unsigned long long)h); return; }
        e = it->second;
        g_reg.erase(it);
        if ((uintptr_t)g_master == (uintptr_t)h) g_master = nullptr;
    }
    switch (e.kind) {
        case Kind::Engine: ((IXAudio2*)(uintptr_t)h)->Release(); break;
        case Kind::Master: ((IXAudio2MasteringVoice*)(uintptr_t)h)->DestroyVoice(); break;
        case Kind::Source:
            ((IXAudio2SourceVoice*)(uintptr_t)h)->DestroyVoice();   // synchronous: no more callbacks after this
            delete e.cb;
            break;
    }
}

xrl_audio_ops g_ops = {
    sizeof(xrl_audio_ops),
    op_xaudio2_create,
    op_create_mastering,
    op_create_source,
    op_submit,
    op_start,
    op_stop,
    op_release,
};

}  // namespace

extern "C" const xrl_audio_ops* xrl_host_audio_ops(void) { return &g_ops; }
