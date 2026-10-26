/* xrl_audio_test.c -- a Windows program that plays a tone through WASAPI shared mode.
 * Built with mingw (wine/tests/build-pe-tests.sh), run as the main program of Wine on the layer:
 *
 *   xrl_audio_test.exe [--seconds N] [--amp A] [--only N] [--list]
 *
 * Runs a few streams against the default render endpoint (Wine's mmdevapi, whose driver
 * winexrl.drv hands the audio to the host's xrl_audio_ops):
 *   0  event-driven, the endpoint's mix format (float, stereo, 48 kHz)
 *   1  polling (no event), the mix format
 *   2  event-driven, 16-bit PCM stereo 48 kHz when the endpoint accepts it (else skipped)
 *   3  a format the endpoint cannot play (12 channels): IsFormatSupported says S_FALSE and offers
 *      the mix format, and Initialize fails
 *   4  stop / reset / start again on one stream
 *   6  event-driven 24-bit and 8-bit PCM (the driver converts them to float for the host)
 *   5  XAudio2 (Wine's FAudio-based xaudio2_9.dll, which plays through WASAPI): a one-second tone
 *      on a source voice; the voice must consume its buffer in real time
 * For every stream it prints how many frames were written, how long that took on the wall clock
 * against how long the audio lasts (the driver paces the app by the host's consumption, so the
 * two must agree), the stream position at the end, and the padding seen.  The host side (the
 * test host's audio log lines) shows the buffers and their peak level arriving.
 * Exit code 0 when every stream behaved.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <xaudio2.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

static const GUID subtype_pcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static double seconds = 2.0, amplitude = 0.1;
static int failures;

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)
#define STEP(...) do { printf("  " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static double now_s(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
}

struct tone
{
    double phase, step;
    int kind;          /* 0 float32, 1 pcm16, 2 pcm8, 3 pcm24, 4 pcm32 */
    int channels;
};

static void fill(struct tone *t, BYTE *dst, UINT32 frames)
{
    UINT32 i;
    int c;

    for (i = 0; i < frames; i++)
    {
        double v = sin(t->phase) * amplitude;
        t->phase += t->step;
        if (t->phase > 2 * PI) t->phase -= 2 * PI;
        for (c = 0; c < t->channels; c++)
        {
            BYTE *p = dst + ((size_t)i * t->channels + c) * (t->kind == 0 ? 4 : t->kind == 1 ? 2 : t->kind == 2 ? 1 : t->kind == 3 ? 3 : 4);
            INT32 n;
            switch (t->kind)
            {
            case 0: *(float *)p = (float)v; break;
            case 1: *(SHORT *)p = (SHORT)(v * 32767.0); break;
            case 2: *p = (BYTE)(128 + (int)(v * 127.0)); break;
            case 3: n = (INT32)(v * 8388607.0); p[0] = n & 0xff; p[1] = (n >> 8) & 0xff; p[2] = (n >> 16) & 0xff; break;
            default: *(INT32 *)p = (INT32)(v * 2147483647.0); break;
            }
        }
    }
}

static IAudioClient *open_client(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { printf("  FAIL CoCreateInstance(MMDeviceEnumerator): %#lx\n", hr); return NULL; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) { printf("  FAIL GetDefaultAudioEndpoint: %#lx\n", hr); return NULL; }
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    IMMDevice_Release(dev);
    if (FAILED(hr)) { printf("  FAIL Activate(IAudioClient): %#lx\n", hr); return NULL; }
    return client;
}

struct result
{
    UINT64 frames;
    double wall, audio;
    UINT64 position, freq;
    UINT32 max_padding, buffer_frames, latency_frames;
    unsigned events;
};

/* Plays the tone for `seconds`; `event` mode waits on the stream's event, otherwise polls. */
static HRESULT play(IAudioClient *client, const WAVEFORMATEX *fmt, int use_event, struct result *r)
{
    IAudioRenderClient *render = NULL;
    IAudioClock *clock = NULL;
    HANDLE event = NULL;
    struct tone t = { 0, 2 * PI * 440.0 / fmt->nSamplesPerSec, 0, fmt->nChannels };
    UINT32 bufsize, padding, avail;
    UINT64 want, written = 0;
    REFERENCE_TIME def, min;
    BYTE *data;
    double t0, t1;
    HRESULT hr;

    memset(r, 0, sizeof(*r));
    if (fmt->wFormatTag == WAVE_FORMAT_PCM || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        IsEqualGUID(&((const WAVEFORMATEXTENSIBLE *)fmt)->SubFormat, &subtype_pcm)))
        t.kind = fmt->wBitsPerSample == 8 ? 2 : fmt->wBitsPerSample == 16 ? 1 : fmt->wBitsPerSample == 24 ? 3 : 4;

    IAudioClient_GetDevicePeriod(client, &def, &min);
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, use_event ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0,
                                 0, 0, fmt, NULL);
    STEP("Initialize(%s, %lu Hz %u ch %u bit) -> %#lx (device period %.1f ms, minimum %.1f ms)",
         use_event ? "event" : "poll", fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample, hr, def / 10000.0, min / 10000.0);
    if (FAILED(hr)) return hr;
    if (use_event)
    {
        event = CreateEventW(NULL, FALSE, FALSE, NULL);
        CHECK(SUCCEEDED(hr = IAudioClient_SetEventHandle(client, event)), "SetEventHandle %#lx", hr);
    }
    if (FAILED(hr = IAudioClient_GetBufferSize(client, &bufsize))) return hr;
    r->buffer_frames = bufsize;
    {
        REFERENCE_TIME lat = 0;
        IAudioClient_GetStreamLatency(client, &lat);
        r->latency_frames = (UINT32)(lat * fmt->nSamplesPerSec / 10000000);
    }
    if (FAILED(hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render))) return hr;
    if (FAILED(hr = IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock))) return hr;
    IAudioClock_GetFrequency(clock, &r->freq);

    /* prefill the whole buffer, as an app does before Start */
    hr = IAudioRenderClient_GetBuffer(render, bufsize, &data);
    if (FAILED(hr)) { printf("  FAIL GetBuffer(prefill): %#lx\n", hr); return hr; }
    fill(&t, data, bufsize);
    IAudioRenderClient_ReleaseBuffer(render, bufsize, 0);
    written = bufsize;

    want = (UINT64)(seconds * fmt->nSamplesPerSec) + bufsize;
    t0 = now_s();
    if (FAILED(hr = IAudioClient_Start(client))) { printf("  FAIL Start: %#lx\n", hr); return hr; }
    while (written < want)
    {
        if (use_event)
        {
            if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) { printf("  FAIL no event for 2 s\n"); hr = E_FAIL; break; }
            r->events++;
        }
        else Sleep(5);
        IAudioClient_GetCurrentPadding(client, &padding);
        if (padding > r->max_padding) r->max_padding = padding;
        avail = bufsize - padding;
        if (!avail) continue;
        if (written + avail > want) avail = (UINT32)(want - written);
        hr = IAudioRenderClient_GetBuffer(render, avail, &data);
        if (FAILED(hr)) { printf("  FAIL GetBuffer(%u): %#lx\n", avail, hr); break; }
        fill(&t, data, avail);
        IAudioRenderClient_ReleaseBuffer(render, avail, 0);
        written += avail;
    }
    t1 = now_s();
    IAudioClock_GetPosition(clock, &r->position, NULL);
    IAudioClient_Stop(client);
    r->frames = written;
    r->wall = t1 - t0;
    /* what is still in the endpoint when the last write returns (buffer + hidden latency) is not played yet */
    r->audio = (double)(written - bufsize - r->latency_frames) / fmt->nSamplesPerSec;
    IAudioClock_Release(clock);
    IAudioRenderClient_Release(render);
    if (event) CloseHandle(event);
    return hr;
}

static void report(const char *what, const struct result *r, const WAVEFORMATEX *fmt)
{
    double ratio = r->audio > 0 ? r->wall / r->audio : 0;
    UINT64 pos_frames = r->freq ? r->position / (r->freq / fmt->nSamplesPerSec) : 0;

    STEP("%s: %llu frames written (buffer %u, stream latency %u), %u events, max padding %u", what,
         (unsigned long long)r->frames, r->buffer_frames, r->latency_frames, r->events, r->max_padding);
    STEP("%s: wall %.3f s for %.3f s of audio (ratio %.3f); position %llu frames", what, r->wall, r->audio, ratio,
         (unsigned long long)pos_frames);
    CHECK(ratio > 0.9 && ratio < 1.1, "playback was not paced at the real-time rate (ratio %.3f)", ratio);
    CHECK(pos_frames > 0 && pos_frames <= r->frames, "position %llu outside 0..%llu", (unsigned long long)pos_frames,
          (unsigned long long)r->frames);
}

static void run(int n)
{
    IAudioClient *client;
    WAVEFORMATEX *mix = NULL, *closest = NULL;
    struct result r;
    HRESULT hr;

    printf("[%d]\n", n);
    fflush(stdout);
    if (!(client = open_client())) { failures++; return; }
    if (FAILED(hr = IAudioClient_GetMixFormat(client, &mix))) { printf("  FAIL GetMixFormat: %#lx\n", hr); failures++; return; }
    STEP("mix format: tag %#x, %lu Hz, %u ch, %u bit", mix->wFormatTag, mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample);

    switch (n)
    {
    case 0:
    case 1:
        hr = play(client, mix, n == 0, &r);
        CHECK(SUCCEEDED(hr), "play %#lx", hr);
        if (SUCCEEDED(hr)) report(n == 0 ? "event/float" : "poll/float", &r, mix);
        break;
    case 2:
    {
        WAVEFORMATEX pcm = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
        hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &pcm, &closest);
        STEP("IsFormatSupported(48000 Hz 2 ch 16 bit) -> %#lx", hr);
        if (hr != S_OK) { STEP("16-bit PCM not offered by the endpoint; skipped"); break; }
        hr = play(client, &pcm, 1, &r);
        CHECK(SUCCEEDED(hr), "play %#lx", hr);
        if (SUCCEEDED(hr)) report("event/pcm16", &r, &pcm);
        break;
    }
    case 3:
    {
        WAVEFORMATEXTENSIBLE wide = {0};
        wide.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wide.Format.nChannels = 12;
        wide.Format.nSamplesPerSec = 48000;
        wide.Format.wBitsPerSample = 16;
        wide.Format.nBlockAlign = 24;
        wide.Format.nAvgBytesPerSec = 48000 * 24;
        wide.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wide.Samples.wValidBitsPerSample = 16;
        wide.dwChannelMask = 0xfff;
        wide.SubFormat = subtype_pcm;
        hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &wide.Format, &closest);
        STEP("IsFormatSupported(12 channels) -> %#lx, closest %s", hr, closest ? "given" : "none");
        CHECK(hr == S_FALSE && closest, "12 channels: expected S_FALSE with the mix format (%#lx)", hr);
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, &wide.Format, NULL);
        STEP("Initialize(12 channels) -> %#lx", hr);
        CHECK(FAILED(hr), "Initialize accepted 12 channels");
        break;
    }
    case 4:
    {
        IAudioRenderClient *render = NULL;
        UINT32 bufsize, padding = 0;
        BYTE *data;
        struct tone t = { 0, 2 * PI * 440.0 / mix->nSamplesPerSec, 0, mix->nChannels };

        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mix, NULL);
        CHECK(SUCCEEDED(hr), "Initialize %#lx", hr);
        IAudioClient_GetBufferSize(client, &bufsize);
        IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render);
        IAudioRenderClient_GetBuffer(render, bufsize / 2, &data);
        fill(&t, data, bufsize / 2);
        IAudioRenderClient_ReleaseBuffer(render, bufsize / 2, 0);
        CHECK(SUCCEEDED(hr = IAudioClient_Start(client)), "Start %#lx", hr);
        Sleep(100);
        CHECK(SUCCEEDED(hr = IAudioClient_Stop(client)), "Stop %#lx", hr);
        CHECK(SUCCEEDED(hr = IAudioClient_Reset(client)), "Reset %#lx", hr);
        IAudioClient_GetCurrentPadding(client, &padding);
        STEP("after Stop+Reset: padding %u", padding);
        CHECK(padding == 0, "padding after Reset is %u", padding);
        IAudioRenderClient_GetBuffer(render, bufsize / 4, &data);
        fill(&t, data, bufsize / 4);
        IAudioRenderClient_ReleaseBuffer(render, bufsize / 4, 0);
        CHECK(SUCCEEDED(hr = IAudioClient_Start(client)), "Start again %#lx", hr);
        Sleep(100);
        IAudioClient_Stop(client);
        IAudioRenderClient_Release(render);
        break;
    }
    case 6:
    {
        WAVEFORMATEX pcm24 = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 6, 6, 24, 0 };
        WAVEFORMATEX pcm8 = { WAVE_FORMAT_PCM, 1, 22050, 22050, 1, 8, 0 };
        WAVEFORMATEX *fmts[2] = { &pcm24, &pcm8 };
        IAudioClient *c2;
        int k;

        IAudioClient_Release(client);
        client = NULL;
        for (k = 0; k < 2; k++)
        {
            if (!(c2 = open_client())) { failures++; break; }
            hr = IAudioClient_IsFormatSupported(c2, AUDCLNT_SHAREMODE_SHARED, fmts[k], &closest);
            CHECK(hr == S_OK, "IsFormatSupported(%u bit) %#lx", fmts[k]->wBitsPerSample, hr);
            hr = play(c2, fmts[k], 1, &r);
            CHECK(SUCCEEDED(hr), "play %#lx", hr);
            if (SUCCEEDED(hr)) report(k ? "event/pcm8" : "event/pcm24", &r, fmts[k]);
            IAudioClient_Release(c2);
        }
        break;
    }
    case 5:
    {
        HMODULE xa = LoadLibraryA("xaudio2_9.dll");
        HRESULT (WINAPI *create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
        IXAudio2 *engine = NULL;
        IXAudio2MasteringVoice *master = NULL;
        IXAudio2SourceVoice *voice = NULL;
        WAVEFORMATEX f = { WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 48000 * 4, 4, 32, 0 };
        static float samples[48000];
        XAUDIO2_BUFFER b = {0};
        XAUDIO2_VOICE_STATE st;
        struct tone t = { 0, 2 * PI * 440.0 / 48000, 0, 1 };
        double t0;
        int polls = 0;

        CHECK(xa != NULL, "LoadLibrary(xaudio2_9.dll)");
        if (!xa) break;
        create = (void *)GetProcAddress(xa, "XAudio2Create");
        CHECK(create && SUCCEEDED(hr = create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)), "XAudio2Create %#lx", hr);
        if (!engine) break;
        CHECK(SUCCEEDED(hr = IXAudio2_CreateMasteringVoice(engine, &master, 2, 48000, 0, NULL, NULL, 0)), "CreateMasteringVoice %#lx", hr);
        CHECK(SUCCEEDED(hr = IXAudio2_CreateSourceVoice(engine, &voice, &f, 0, 2.0f, NULL, NULL, NULL)), "CreateSourceVoice %#lx", hr);
        if (!voice) break;
        fill(&t, (BYTE *)samples, 48000);
        b.AudioBytes = sizeof(samples);
        b.pAudioData = (BYTE *)samples;
        b.Flags = XAUDIO2_END_OF_STREAM;
        CHECK(SUCCEEDED(hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &b, NULL)), "SubmitSourceBuffer %#lx", hr);
        t0 = now_s();
        CHECK(SUCCEEDED(hr = IXAudio2SourceVoice_Start(voice, 0, 0)), "Start %#lx", hr);
        do { Sleep(20); IXAudio2SourceVoice_GetState(voice, &st, 0); polls++; }
        while (st.BuffersQueued && now_s() - t0 < 5.0);
        STEP("XAudio2: one second of audio consumed in %.3f s (%d polls), %u buffers left", now_s() - t0, polls, st.BuffersQueued);
        CHECK(!st.BuffersQueued, "the voice never finished its buffer");
        CHECK(now_s() - t0 > 0.85 && now_s() - t0 < 1.6, "consumed in %.3f s, not about real time", now_s() - t0);
        IXAudio2SourceVoice_DestroyVoice(voice);
        IXAudio2MasteringVoice_DestroyVoice(master);
        IXAudio2_Release(engine);
        break;
    }
    }
    CoTaskMemFree(mix);
    CoTaskMemFree(closest);
    if (client) IAudioClient_Release(client);
    printf("[%d] done\n", n);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int only = -1, i;
    HRESULT hr;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--amp") && i + 1 < argc) amplitude = atof(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = atoi(argv[++i]);
    }
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("CoInitializeEx -> %#lx\n", hr);
    for (i = 0; i <= 6; i++) if (only < 0 || only == i) run(i);
    printf("RESULT: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    fflush(stdout);
    return failures != 0;
}
