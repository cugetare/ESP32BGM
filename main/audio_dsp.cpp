// ============================================================
//  AUDIO DSP
//  Per-frame chain on 16-bit stereo, tuned for an exciter:
//    1. mono downmix (optional, equal-power)
//    2. bass / treble shelves (biquad, from Basmio)
//    3. sound enhancer (from Basmio): psychoacoustic bass harmonics,
//       fullness + presence shelves that fade out at higher volume,
//       plus mid/side widening when playing stereo
//    4. exciter protection: 4th-order high-pass (adjustable)
//    5. resonance notch (adjustable frequency and depth)
//    6. soft limiter
//  Boosts are matched by input attenuation (headroom), since the
//  volume is applied after this chain, in the codec.
//  Parameters change from the web task; the audio task rebuilds the
//  filters itself when it sees a new revision, so there is no race.
// ============================================================
#include "app_state.h"

#include <math.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"

// ============================================================
//  BIQUAD  (RBJ Audio EQ Cookbook, transposed direct form II)
// ============================================================
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1[2] = { 0, 0 };
  float z2[2] = { 0, 0 };
  bool bypass = true;

  inline float process(float x, int ch) {
    if (bypass) return x;
    float y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
  }

  void identity() {
    b0 = 1; b1 = b2 = a1 = a2 = 0;
    bypass = true;
    z1[0] = z1[1] = z2[0] = z2[1] = 0;
  }

  void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
    if (fabsf(a0) < 1e-9f) {
      identity();
      return;
    }
    b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    bypass = false;
    z1[0] = z1[1] = z2[0] = z2[1] = 0;
  }
};

static bool validFreq(float fs, float f0) {
  return fs > 1000.0f && f0 > 1.0f && f0 < fs * 0.45f;
}

static void lowShelf(Biquad& bq, float fs, float f0, float gainDb) {
  if (!validFreq(fs, f0) || fabsf(gainDb) < 0.01f) { bq.identity(); return; }
  float A = powf(10.0f, gainDb / 40.0f);
  float w0 = 2.0f * (float)M_PI * f0 / fs;
  float c = cosf(w0), alpha = sinf(w0) * 0.5f * sqrtf(2.0f);  // shelf slope S = 1
  float sA = sqrtf(A);
  bq.set(A * ((A + 1) - (A - 1) * c + 2 * sA * alpha),
         2 * A * ((A - 1) - (A + 1) * c),
         A * ((A + 1) - (A - 1) * c - 2 * sA * alpha),
         (A + 1) + (A - 1) * c + 2 * sA * alpha,
         -2 * ((A - 1) + (A + 1) * c),
         (A + 1) + (A - 1) * c - 2 * sA * alpha);
}

static void highShelf(Biquad& bq, float fs, float f0, float gainDb) {
  if (!validFreq(fs, f0) || fabsf(gainDb) < 0.01f) { bq.identity(); return; }
  float A = powf(10.0f, gainDb / 40.0f);
  float w0 = 2.0f * (float)M_PI * f0 / fs;
  float c = cosf(w0), alpha = sinf(w0) * 0.5f * sqrtf(2.0f);
  float sA = sqrtf(A);
  bq.set(A * ((A + 1) + (A - 1) * c + 2 * sA * alpha),
         -2 * A * ((A - 1) + (A + 1) * c),
         A * ((A + 1) + (A - 1) * c - 2 * sA * alpha),
         (A + 1) - (A - 1) * c + 2 * sA * alpha,
         2 * ((A - 1) - (A + 1) * c),
         (A + 1) - (A - 1) * c - 2 * sA * alpha);
}

static void highPass(Biquad& bq, float fs, float f0, float q) {
  if (!validFreq(fs, f0) || q <= 0.01f) { bq.identity(); return; }
  float w0 = 2.0f * (float)M_PI * f0 / fs;
  float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
  bq.set((1 + c) * 0.5f, -(1 + c), (1 + c) * 0.5f, 1 + alpha, -2 * c, 1 - alpha);
}

static void peaking(Biquad& bq, float fs, float f0, float q, float gainDb) {
  if (!validFreq(fs, f0) || fabsf(gainDb) < 0.01f || q <= 0.01f) { bq.identity(); return; }
  float A = powf(10.0f, gainDb / 40.0f);
  float w0 = 2.0f * (float)M_PI * f0 / fs;
  float c = cosf(w0), alpha = sinf(w0) / (2.0f * q);
  bq.set(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}

// ============================================================
//  PARAMETERS
// ============================================================
struct DspParams {
  bool mono;
  int bass, treble;    // -10..10
  int enhancer;        // 0..100
  int protectHz;       // 0 = off
  int notchHz;
  int notchDepthDb;    // 0 = off
  int volume;          // 0..100, drives the loudness-style fade
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static DspParams s_pending = { false, 0, 0, 0, 80, 400, 0, 50 };
static volatile uint32_t s_pendingRev = 1;

void dspApplySettings() {
  DspParams p;
  p.mono = soundSettings.mono;
  p.bass = std::clamp(globalSettings.bass, -10, 10);
  p.treble = std::clamp(globalSettings.treble, -10, 10);
  p.enhancer = std::clamp(soundSettings.enhancer, 0, 100);
  p.protectHz = std::clamp(soundSettings.protectHz, 0, 400);
  p.notchHz = std::clamp(soundSettings.notchHz, 100, 2000);
  p.notchDepthDb = std::clamp(soundSettings.notchDepthDb, 0, 12);
  p.volume = std::clamp(globalSettings.volume, 0, 100);
  portENTER_CRITICAL(&s_mux);
  s_pending = p;
  s_pendingRev = s_pendingRev + 1;
  portEXIT_CRITICAL(&s_mux);
}

// ============================================================
//  RUNTIME STATE (audio task only)
// ============================================================
static uint32_t s_appliedRev = 0;
static uint32_t s_appliedRate = 0;
static DspParams s_p;

static Biquad s_bass, s_treble;
static Biquad s_fullness, s_presence;
static Biquad s_protect1, s_protect2;
static Biquad s_notch;

static bool  s_enhancerOn = false;
static float s_harmonicLpAlpha = 0, s_harmonicHpAlpha = 0, s_harmonicMix = 0;
static float s_harmonicLow = 0, s_harmonicPrevX = 0, s_harmonicPrevY = 0;
static float s_postGain = 1.0f;
static float s_width = 1.0f;
static float s_preGain = 1.0f;

// Same mapping as Basmio: Warm (0-40), Full (40-75), Punch (75-100).
static float enhancerProfile(int amount) {
  float profile;
  if (amount <= 40) profile = (amount / 40.0f) * 0.45f;
  else if (amount <= 75) profile = 0.45f + ((amount - 40.0f) / 35.0f) * 0.35f;
  else profile = 0.80f + ((amount - 75.0f) / 25.0f) * 0.32f;
  return std::min(profile, 1.12f);
}

static void rebuild(uint32_t sampleRate) {
  const float fs = (float)sampleRate;

  // Basmio tone mapping: full scale = +/-12 dB.
  lowShelf(s_bass, fs, 120.0f, s_p.bass * 1.2f);
  highShelf(s_treble, fs, 6000.0f, s_p.treble * 1.2f);

  // Enhancer "depth" (harmonics + fullness + presence) backs off as the
  // volume goes up, like loudness compensation: full below 45 %, gone at 72 %.
  float profile = enhancerProfile(s_p.enhancer);
  float lowVolume = 0.0f;
  if (s_p.volume <= 45) lowVolume = 1.0f;
  else if (s_p.volume < 72) lowVolume = (72.0f - s_p.volume) / 27.0f;
  float strength = profile * lowVolume;

  s_enhancerOn = s_p.enhancer > 0;
  if (s_enhancerOn && strength > 0.005f) {
    lowShelf(s_fullness, fs, 175.0f, 10.5f * strength);
    highShelf(s_presence, fs, 4800.0f, 3.6f * strength);
  } else {
    s_fullness.identity();
    s_presence.identity();
  }
  s_harmonicLpAlpha = 1.0f - expf(-2.0f * (float)M_PI * 125.0f / fs);
  float hpRc = 1.0f / (2.0f * (float)M_PI * 78.0f);
  s_harmonicHpAlpha = hpRc / (hpRc + 1.0f / fs);
  s_harmonicMix = 0.38f * strength;
  s_harmonicLow = s_harmonicPrevX = s_harmonicPrevY = 0.0f;
  s_postGain = 1.0f + 0.15f * strength;

  // "Wide": side boost up to +50 %, independent of volume, stereo only.
  s_width = (s_enhancerOn && !s_p.mono) ? 1.0f + 0.5f * (s_p.enhancer / 100.0f) : 1.0f;

  // Headroom: volume is applied after the DSP (in the ES8388), so boosts act
  // on full-scale audio. Pre-attenuate by the largest active boost so a
  // loud master doesn't clip; turn the volume up to compensate.
  float bassDb = std::max(0.0f, s_p.bass * 1.2f);
  float trebleDb = std::max(0.0f, s_p.treble * 1.2f);
  float fullDb = s_enhancerOn ? 10.5f * strength : 0.0f;
  float presDb = s_enhancerOn ? 3.6f * strength : 0.0f;
  float postDb = 20.0f * log10f(s_postGain);
  float boostDb = std::max(bassDb + fullDb, trebleDb + presDb) + postDb;
  s_preGain = powf(10.0f, -boostDb / 20.0f);

  // Exciter protection: 4th-order Butterworth high-pass (two biquads).
  if (s_p.protectHz > 0) {
    highPass(s_protect1, fs, (float)s_p.protectHz, 0.5412f);
    highPass(s_protect2, fs, (float)s_p.protectHz, 1.3066f);
  } else {
    s_protect1.identity();
    s_protect2.identity();
  }

  // Resonance notch: narrow cut at the exciter / panel resonance.
  peaking(s_notch, fs, (float)s_p.notchHz, 2.5f, -(float)s_p.notchDepthDb);
}

static inline float softLimit(float x) {
  const float threshold = 30000.0f;
  float ax = fabsf(x);
  if (ax <= threshold) return x;
  float over = (ax - threshold) / (32767.0f - threshold);
  ax = threshold + (32767.0f - threshold) * (over / (1.0f + over));
  return x < 0 ? -ax : ax;
}

static inline int16_t toI16(float x) {
  if (x > 32767.0f) return 32767;
  if (x < -32768.0f) return -32768;
  return (int16_t)x;
}

void dspProcess(int16_t* pcm, size_t frames, uint32_t sampleRate) {
  if (s_appliedRev != s_pendingRev || s_appliedRate != sampleRate) {
    portENTER_CRITICAL(&s_mux);
    s_p = s_pending;
    s_appliedRev = s_pendingRev;
    portEXIT_CRITICAL(&s_mux);
    s_appliedRate = sampleRate;
    rebuild(sampleRate);
  }

  const bool mono = s_p.mono;
  const bool enhance = s_enhancerOn;
  for (size_t i = 0; i < frames; i++) {
    float l = pcm[i * 2];
    float r = pcm[i * 2 + 1];

    if (mono) {
      l = r = (l + r) * 0.70710678f;  // equal-power downmix
    }
    l *= s_preGain;
    r *= s_preGain;

    l = s_treble.process(s_bass.process(l, 0), 0);
    r = s_treble.process(s_bass.process(r, 1), 1);

    if (enhance) {
      // Bass harmonics: rectify the low band, keep what's above ~78 Hz.
      // The ear hears the fundamental the exciter can't move air for.
      float mid = (l + r) * 0.5f;
      s_harmonicLow += s_harmonicLpAlpha * (mid - s_harmonicLow);
      float generated = fabsf(s_harmonicLow) > 280.0f ? fabsf(s_harmonicLow) - 280.0f : 0.0f;
      float harmonic = s_harmonicHpAlpha * (s_harmonicPrevY + generated - s_harmonicPrevX);
      s_harmonicPrevX = generated;
      s_harmonicPrevY = harmonic;
      l += harmonic * s_harmonicMix;
      r += harmonic * s_harmonicMix;

      l = s_presence.process(s_fullness.process(l, 0), 0);
      r = s_presence.process(s_fullness.process(r, 1), 1);

      if (s_width != 1.0f) {
        float m = (l + r) * 0.5f;
        float s = (l - r) * 0.5f * s_width;
        l = m + s;
        r = m - s;
      }
      l *= s_postGain;
      r *= s_postGain;
    }

    l = s_notch.process(s_protect2.process(s_protect1.process(l, 0), 0), 0);
    r = s_notch.process(s_protect2.process(s_protect1.process(r, 1), 1), 1);

    pcm[i * 2] = toI16(softLimit(l));
    pcm[i * 2 + 1] = toI16(softLimit(r));
  }
}
