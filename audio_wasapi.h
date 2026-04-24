#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// audio_wasapi.h — WASAPI loopback capture (records what's playing through the
// default audio render endpoint, i.e. "system sound" / "what you hear").
// Used as an alternative to waveIn (microphone) capture. Output is 16-bit PCM
// at the caller-requested sample rate / channel count.
// ═══════════════════════════════════════════════════════════════════════════

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstdint>
#include <vector>
#include <string>
#include <atomic>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace audio_wasapi {

// Minimal wrapper for COM pointers — auto-release on destruction.
template<typename T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
    T* release() { T* r = p; p = nullptr; return r; }
};

// Simple resampler (linear interpolation) from src_rate→dst_rate, used only
// when the device mix format doesn't match the caller's desired rate.
static inline void resample_linear_i16(const int16_t* src, int src_frames, int src_rate,
                                       int16_t* dst, int dst_frames_cap, int dst_rate,
                                       int channels, int* out_frames)
{
    if (src_rate == dst_rate) {
        int n = src_frames < dst_frames_cap ? src_frames : dst_frames_cap;
        memcpy(dst, src, size_t(n) * channels * sizeof(int16_t));
        *out_frames = n;
        return;
    }
    double ratio = double(src_rate) / double(dst_rate);
    int produced = 0;
    for (int i = 0; i < dst_frames_cap; ++i) {
        double sp = i * ratio;
        int sf = (int)sp;
        if (sf + 1 >= src_frames) break;
        double frac = sp - sf;
        for (int ch = 0; ch < channels; ++ch) {
            int a = src[sf * channels + ch];
            int b = src[(sf + 1) * channels + ch];
            dst[i * channels + ch] = (int16_t)(a + (b - a) * frac);
        }
        ++produced;
    }
    *out_frames = produced;
}

// ══════════════════════════════════════════════════════════════════════════
// Capture class. Opens the default render endpoint in loopback mode, delivers
// 16-bit PCM in the caller's requested format (auto-downmix to mono, auto-
// resample). Uses event-driven IAudioClient for low latency.
// ══════════════════════════════════════════════════════════════════════════
class Capture {
public:
    // Open the default render endpoint in loopback mode.
    //   wantRate     : desired sample rate (e.g. 48000); kept for resampling
    //   wantChannels : desired channel count (1 = mono downmix, 2 = stereo passthrough)
    //   err          : human-readable error if returns false
    bool open(int wantRate, int wantChannels, std::string* err = nullptr) {
        close();
        want_rate_ = wantRate;
        want_ch_   = wantChannels;

        // COM is per-thread. We use COINIT_MULTITHREADED so the client can
        // be used from any background thread.
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        co_owned_ = SUCCEEDED(hr);   // may legitimately return S_FALSE if
                                     // already initialized by the caller

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              (void**)&enumerator);
        if (FAILED(hr)) { if (err) *err = "CoCreateInstance(MMDeviceEnumerator) failed"; return false; }

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr)) { if (err) *err = "GetDefaultAudioEndpoint(eRender) failed"; return false; }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               (void**)&client_);
        if (FAILED(hr)) { if (err) *err = "IMMDevice::Activate(IAudioClient) failed"; return false; }

        // Get the mix format the endpoint actually wants. Usually 48 kHz
        // float32 stereo (shared mode). We always capture at that format
        // and convert to int16 in read().
        hr = client_->GetMixFormat(&mix_fmt_);
        if (FAILED(hr) || !mix_fmt_) { if (err) *err = "IAudioClient::GetMixFormat failed"; return false; }

        // Buffer size: 200ms. Short enough for low latency, long enough that
        // we don't starve if the reader thread stalls briefly.
        REFERENCE_TIME bufDur = 2000000; // 200 ms in 100ns units
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK |
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 bufDur, 0, mix_fmt_, nullptr);
        if (FAILED(hr)) {
            // EVENTCALLBACK sometimes fails in loopback mode on older drivers
            // — fall back to pure LOOPBACK (pull model, no event).
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     bufDur, 0, mix_fmt_, nullptr);
            if (FAILED(hr)) { if (err) *err = "IAudioClient::Initialize(LOOPBACK) failed"; return false; }
        } else {
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            client_->SetEventHandle(event_);
        }

        hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_);
        if (FAILED(hr)) { if (err) *err = "GetService(IAudioCaptureClient) failed"; return false; }

        hr = client_->Start();
        if (FAILED(hr)) { if (err) *err = "IAudioClient::Start failed"; return false; }

        running_ = true;
        return true;
    }

    // Pump all available packets into `out` (appends int16 samples, interleaved
    // by channels, in the caller's wantRate/wantChannels format). Returns
    // number of FRAMES appended (not samples). Non-blocking.
    //
    // If the WASAPI event handle exists, the caller should wait on event()
    // before calling read() for efficient CPU use. If not, poll with ~10ms
    // Sleep between calls.
    int read(std::vector<int16_t>& out) {
        if (!running_ || !capture_) return 0;
        int totalFrames = 0;
        UINT32 packetLen = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packetLen);
        while (SUCCEEDED(hr) && packetLen > 0) {
            BYTE*  data = nullptr;
            UINT32 frames = 0;
            DWORD  flags = 0;
            UINT64 pos = 0, qpc = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &pos, &qpc);
            if (FAILED(hr)) break;

            if (frames > 0) {
                // Convert source frames -> int16 at mix format rate/channels
                // first, then resample/downmix to requested format.
                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                std::vector<int16_t> tmp;
                tmp.resize(size_t(frames) * mix_fmt_->nChannels);

                if (silent) {
                    // Silence: zero-fill, no need to decode
                    std::fill(tmp.begin(), tmp.end(), (int16_t)0);
                } else {
                    convert_to_i16(data, frames, tmp.data());
                }

                // If mix format matches wanted format, append directly.
                if ((int)mix_fmt_->nSamplesPerSec == want_rate_ &&
                    (int)mix_fmt_->nChannels     == want_ch_) {
                    out.insert(out.end(), tmp.begin(), tmp.end());
                    totalFrames += frames;
                } else {
                    // Downmix to requested channel count
                    std::vector<int16_t> mono;
                    const int srcCh = mix_fmt_->nChannels;
                    if (srcCh != want_ch_) {
                        if (want_ch_ == 1 && srcCh >= 2) {
                            mono.resize(frames);
                            for (UINT32 i = 0; i < frames; ++i) {
                                int sum = 0;
                                for (int c = 0; c < srcCh; ++c)
                                    sum += tmp[i * srcCh + c];
                                mono[i] = (int16_t)(sum / srcCh);
                            }
                        } else if (want_ch_ == 2 && srcCh == 1) {
                            mono.resize(size_t(frames) * 2);
                            for (UINT32 i = 0; i < frames; ++i) {
                                mono[i*2]   = tmp[i];
                                mono[i*2+1] = tmp[i];
                            }
                        } else {
                            mono = tmp; // fallback: use as-is
                        }
                    } else {
                        mono = std::move(tmp);
                    }

                    // Resample if needed
                    if ((int)mix_fmt_->nSamplesPerSec != want_rate_) {
                        int dstCap = (int)(double(frames) * double(want_rate_) /
                                           double(mix_fmt_->nSamplesPerSec)) + 4;
                        std::vector<int16_t> rs(size_t(dstCap) * want_ch_);
                        int produced = 0;
                        resample_linear_i16(mono.data(), frames,
                                            (int)mix_fmt_->nSamplesPerSec,
                                            rs.data(), dstCap, want_rate_,
                                            want_ch_, &produced);
                        out.insert(out.end(), rs.begin(),
                                   rs.begin() + size_t(produced) * want_ch_);
                        totalFrames += produced;
                    } else {
                        out.insert(out.end(), mono.begin(), mono.end());
                        totalFrames += frames;
                    }
                }
            }
            capture_->ReleaseBuffer(frames);
            hr = capture_->GetNextPacketSize(&packetLen);
        }
        return totalFrames;
    }

    HANDLE event() const { return event_; }

    void close() {
        if (client_ && running_) { client_->Stop(); }
        running_ = false;
        if (capture_)     { capture_->Release();   capture_  = nullptr; }
        if (client_)      { client_->Release();    client_   = nullptr; }
        if (device_)      { device_->Release();    device_   = nullptr; }
        if (mix_fmt_)     { CoTaskMemFree(mix_fmt_); mix_fmt_ = nullptr; }
        if (event_)       { CloseHandle(event_);   event_    = nullptr; }
        if (co_owned_)    { CoUninitialize();      co_owned_ = false; }
    }

    ~Capture() { close(); }

private:
    // Convert `frames` frames in the device mix format (stored in mix_fmt_)
    // to int16 PCM. Supports float32, int16, int32, int24-in-32 samples.
    void convert_to_i16(const BYTE* src, UINT32 frames, int16_t* dst) {
        const int ch = mix_fmt_->nChannels;
        const WORD bits = mix_fmt_->wBitsPerSample;
        const WORD tag = mix_fmt_->wFormatTag;

        // WAVE_FORMAT_EXTENSIBLE carries the real subformat in SubFormat GUID.
        bool isFloat = false;
        if (tag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (tag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt_);
            if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                isFloat = true;
        }

        const UINT32 samples = frames * ch;

        if (isFloat && bits == 32) {
            const float* f = reinterpret_cast<const float*>(src);
            for (UINT32 i = 0; i < samples; ++i) {
                float v = f[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                dst[i] = (int16_t)(v * 32767.0f);
            }
        } else if (bits == 16) {
            memcpy(dst, src, size_t(samples) * sizeof(int16_t));
        } else if (bits == 32) {
            // int32 PCM — take upper 16 bits
            const int32_t* s = reinterpret_cast<const int32_t*>(src);
            for (UINT32 i = 0; i < samples; ++i)
                dst[i] = (int16_t)(s[i] >> 16);
        } else if (bits == 24) {
            // 24-bit packed little-endian
            for (UINT32 i = 0; i < samples; ++i) {
                int32_t v = (int32_t(src[i*3])      ) |
                            (int32_t(src[i*3 + 1]) <<  8) |
                            (int32_t(src[i*3 + 2]) << 16);
                if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend
                dst[i] = (int16_t)(v >> 8);         // 24→16 bit
            }
        } else {
            // Unknown format — zero-fill so we don't emit garbage
            memset(dst, 0, size_t(samples) * sizeof(int16_t));
        }
    }

    IMMDevice*            device_   = nullptr;
    IAudioClient*         client_   = nullptr;
    IAudioCaptureClient*  capture_  = nullptr;
    WAVEFORMATEX*         mix_fmt_  = nullptr;
    HANDLE                event_    = nullptr;
    bool                  co_owned_ = false;
    bool                  running_  = false;
    int                   want_rate_ = 48000;
    int                   want_ch_   = 1;
};

} // namespace audio_wasapi
