#pragma once
// ═══════════════════════════════════════════════════════════════
// audio_dsp.h — lightweight in-place DSP for 16-bit PCM
// - High-pass filter (removes DC/low-freq rumble)
// - Noise gate with hysteresis (attenuates quiet sections)
// - Peak normalization (scales to target peak level)
//
// Operates on interleaved int16_t samples. No heap allocations,
// no external deps. Designed to run on every captured buffer
// before the Opus encoder.
// ═══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

namespace audio_dsp {

// ── 1-pole high-pass filter state (per channel) ──
// y[n] = alpha * (y[n-1] + x[n] - x[n-1])
// alpha = RC / (RC + dt),  fc ≈ 80 Hz by default
struct HighPassState {
    float prev_in[2]  = {0.f, 0.f};  // up to stereo
    float prev_out[2] = {0.f, 0.f};
    float alpha       = 0.f;
    int   channels    = 1;

    void configure(int sample_rate, int ch, float cutoff_hz = 80.f) {
        channels = (ch == 2 ? 2 : 1);
        float rc = 1.0f / (2.0f * 3.14159265f * cutoff_hz);
        float dt = 1.0f / (float)sample_rate;
        alpha    = rc / (rc + dt);
        prev_in[0] = prev_in[1] = prev_out[0] = prev_out[1] = 0.f;
    }
};

inline void apply_highpass(int16_t* samples, int num_samples, HighPassState& st) {
    if (st.alpha == 0.f) return;
    int frames = num_samples / st.channels;
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < st.channels; c++) {
            float x = (float)samples[i * st.channels + c];
            float y = st.alpha * (st.prev_out[c] + x - st.prev_in[c]);
            st.prev_in[c]  = x;
            st.prev_out[c] = y;
            int v = (int)y;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            samples[i * st.channels + c] = (int16_t)v;
        }
    }
}

// ── Noise gate with per-buffer adaptive floor ──
// Two-pass batch operation:
//   Pass 1: compute RMS for each ~20ms window, pick the 15th percentile as
//           noise floor. This adapts to the actual loudness of the recording
//           (works for both whispered and shouted content).
//   Pass 2: gate windows whose RMS < floor * open_margin, attenuate to 40%
//           (not muted — preserves "breath" and avoids hard transients).
// Hysteresis + hold time smooths brief dips below the threshold.
struct NoiseGateState {
    int sample_rate = 16000;
    void configure(int sr) { sample_rate = sr; }
};

inline void apply_noise_gate(int16_t* samples, int num_samples, int channels,
                              NoiseGateState& st,
                              float open_margin  = 2.2f,   // open when RMS > floor * 2.2
                              float close_margin = 1.4f,   // close when RMS < floor * 1.4
                              int   hold_ms      = 200) {
    if (num_samples <= 0 || channels <= 0) return;
    const int frames = num_samples / channels;
    if (frames <= 4) return;

    const int window_frames = std::max(1, st.sample_rate / 50); // ~20 ms
    const int hold_frames   = st.sample_rate * hold_ms / 1000;

    // ── Pass 1: RMS per window ──
    int num_windows = (frames + window_frames - 1) / window_frames;
    std::vector<float> rms_vals;
    rms_vals.reserve(num_windows);
    for (int w = 0; w < frames; w += window_frames) {
        int   w_end = std::min(w + window_frames, frames);
        double s = 0.0;
        int    n = 0;
        for (int i = w; i < w_end; i++) {
            for (int c = 0; c < channels; c++) {
                float v = (float)samples[i * channels + c];
                s += v * v;
                n++;
            }
        }
        rms_vals.push_back(n > 0 ? (float)std::sqrt(s / n) : 0.f);
    }
    if (rms_vals.empty()) return;

    // Sort a copy; pick 15th-percentile as noise floor
    std::vector<float> sorted_rms = rms_vals;
    std::sort(sorted_rms.begin(), sorted_rms.end());
    size_t idx = std::min(sorted_rms.size() - 1, sorted_rms.size() * 15 / 100);
    float floor_est = sorted_rms[idx];

    // Also compute the peak RMS — if the ratio peak/floor is small (<4),
    // the recording is essentially all noise OR all signal at uniform level;
    // in that case disable gating to avoid muting everything.
    float peak_rms = sorted_rms.back();
    if (floor_est < 20.f) floor_est = 20.f;
    if (peak_rms / floor_est < 4.0f) {
        return; // no clear signal/noise separation — pass through
    }

    // ── Pass 2: apply gate ──
    float open_thresh  = floor_est * open_margin;
    float close_thresh = floor_est * close_margin;
    bool  gate_open    = false;
    int   hold_left    = 0;

    for (size_t wi = 0; wi < rms_vals.size(); wi++) {
        int w     = (int)(wi * window_frames);
        int w_end = std::min(w + window_frames, frames);
        float rms = rms_vals[wi];

        if (rms > open_thresh) {
            gate_open = true;
            hold_left = hold_frames;
        } else if (gate_open && rms < close_thresh) {
            hold_left -= (w_end - w);
            if (hold_left <= 0) gate_open = false;
        }

        if (!gate_open) {
            // Attenuate to 40% — audible but reduced; avoids apparent silence
            // and nasty transitions at the gate boundary.
            for (int i = w; i < w_end; i++) {
                for (int c = 0; c < channels; c++) {
                    int v = (samples[i * channels + c] * 2) / 5; // ×0.4
                    samples[i * channels + c] = (int16_t)v;
                }
            }
        }
    }
}

// ── Biquad notch filter (RBJ cookbook) ──
// Removes a narrow frequency band around f0. Used for power-line hum
// at 50/60 Hz and harmonics. Q controls bandwidth: higher Q = narrower notch.
// Cascade multiple instances for multiple harmonics.
struct NotchState {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    // Per-channel delay lines
    float x1[2] = {0.f, 0.f}, x2[2] = {0.f, 0.f};
    float y1[2] = {0.f, 0.f}, y2[2] = {0.f, 0.f};
    int channels = 1;

    void configure(int sample_rate, float f0_hz, float Q, int ch) {
        channels = (ch == 2 ? 2 : 1);
        float w0 = 2.0f * 3.14159265f * f0_hz / (float)sample_rate;
        float cw = std::cos(w0);
        float sw = std::sin(w0);
        float alpha = sw / (2.0f * Q);
        float a0 = 1.0f + alpha;
        b0 =  1.0f / a0;
        b1 = -2.0f * cw / a0;
        b2 =  1.0f / a0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
        x1[0] = x1[1] = x2[0] = x2[1] = 0.f;
        y1[0] = y1[1] = y2[0] = y2[1] = 0.f;
    }
};

inline void apply_notch(int16_t* samples, int num_samples, NotchState& st) {
    int frames = num_samples / st.channels;
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < st.channels; c++) {
            float x = (float)samples[i * st.channels + c];
            float y = st.b0 * x + st.b1 * st.x1[c] + st.b2 * st.x2[c]
                    - st.a1 * st.y1[c] - st.a2 * st.y2[c];
            st.x2[c] = st.x1[c]; st.x1[c] = x;
            st.y2[c] = st.y1[c]; st.y1[c] = y;
            int v = (int)y;
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            samples[i * st.channels + c] = (int16_t)v;
        }
    }
}

// Cascade: apply notch at base_freq and its harmonics (2x, 3x).
// Q=10 = moderately narrow, low ringing, robust against float32 precision
// issues. Only 3 harmonics to keep group delay / ringing minimal.
struct HumFilterBank {
    NotchState notches[3];
    int active_count = 0;

    void configure(int sample_rate, float base_hz, int channels, float Q = 10.f) {
        active_count = 0;
        float nyquist = (float)sample_rate * 0.5f;
        for (int h = 1; h <= 3; h++) {
            float f = base_hz * (float)h;
            if (f >= nyquist - 50.f) break;
            notches[active_count++].configure(sample_rate, f, Q, channels);
        }
    }
};

inline void apply_hum_filter(int16_t* samples, int num_samples, HumFilterBank& bank) {
    for (int i = 0; i < bank.active_count; i++) {
        apply_notch(samples, num_samples, bank.notches[i]);
    }
}

// ── Peak normalization to target level ──
// Scans buffer for peak, scales so peak == target (default 0.9 * int16_max).
// Safe for short buffers; scale is clamped to max_scale to avoid blowing up
// pure silence or very quiet recordings.
inline void apply_normalize(int16_t* samples, int num_samples,
                             float target_ratio = 0.90f,
                             float max_scale    = 8.0f) {
    if (num_samples <= 0) return;
    int peak = 0;
    for (int i = 0; i < num_samples; i++) {
        int v = samples[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (peak < 500) return; // near-silence: leave alone
    float target = 32767.f * target_ratio;
    float scale  = target / (float)peak;
    if (scale > max_scale) scale = max_scale;
    if (scale < 1.01f) return; // already close to target — don't touch
    for (int i = 0; i < num_samples; i++) {
        int v = (int)((float)samples[i] * scale);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        samples[i] = (int16_t)v;
    }
}

} // namespace audio_dsp
