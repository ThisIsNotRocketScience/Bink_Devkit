#pragma once
#include "StereoDelay.h"
#define _USE_MATH_DEFINES
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// __builtin_clz isn't available on MSVC — provide a shim.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline int __builtin_clz(unsigned int x) {
  unsigned long r;
  _BitScanReverse(&r, x);
  return 31 - (int)r;
}
#endif

constexpr int kNumVoices = 12;
constexpr int kWavetableSize = 2048; // must be a power of two
constexpr int kWavetableBits = 11;   // log2(kWavetableSize)
constexpr float kInvNumVoices = 1.0f / kNumVoices;
constexpr int kPartials = 6; // additive partials per voice (modal/sine)

// Partial-amp smoother runs once every kSmootherBlockSize samples instead of
// per-sample. Decay coefficients stored in SynthVoice are pre-raised to this
// power so the absolute time constant is unchanged. 16 samples @ 48 kHz =
// ~0.33 ms — well below the shortest audible decay.
constexpr int kSmootherBlockSize = 16;
constexpr float kPartialDeadThreshold = 1.0e-4f;

// Number of mip levels: one per octave, covering ~10 octaves.
// Top level (kNumMipLevels-1) holds a single harmonic = pure sine — reused
// by the additive engines below.
constexpr int kNumMipLevels = 11;

// Runtime-settable phase scale + sample rate. Set via
// SimpleSynth::setSampleRate before first noteOn so additive decay coefficients
// land in the right range.
static inline float &gPhaseScaleRef() {
  static float v = 4294967296.0f / 48000.0f;
  return v;
}
#define gPhaseScale (gPhaseScaleRef())

static inline float &gSampleRateRef() {
  static float v = 48000.0f;
  return v;
}
#define gSampleRate (gSampleRateRef())

// these are the 8 knobs for this synth
enum KnobNames {

  KNOB_VOLUME,    // master volume
  KNOB_CUTOFF,    // filter cutoff
  KNOB_RESONANCE, // filter resonance
  KNOB_EFFECT,    // effect mix - different for each of the 4 sounds
  KNOB_TOUCH,     // amount of influence the touch pressure has on sound mod
                  // parameters -> this knob is bipolar. from -1 to 1
  KNOB_VELOCITY, // amount of influence the velocity has on sound mod parameters
  KNOB_CURVE,    // pressure-response curve (sigmoid shaper)
  KNOB_SOUND     // sound preset -> divides the 0 to 1 range in 4 equal spaced
                 // presets

};

// 4 polysynth engines selected by KNOB_SOUND.
enum SynthMode {
  SYNTH_SUBTRACTIVE = 0, // dual-saw + SVF (the original engine)
  SYNTH_MODAL,           // bank of damped inharmonic sines (bell/marimba)
  SYNTH_SAW_MORPH,       // 3-saw morph: base + harmonic crossfade via pressure
  SYNTH_PWM,             // antialiased DPW pulse with pressure-driven PWM
  SYNTH_NUM_MODES = 4
};

#ifdef MIPSAW_USE_PRECOMPUTED_TABLES
#include "MipSawTableData.h"
#endif

class MipSawTable {
public:
#ifndef MIPSAW_USE_PRECOMPUTED_TABLES
  MipSawTable() {
    for (int level = 0; level < kNumMipLevels; ++level) {
      int maxHarmonic = (kWavetableSize / 2) >> level;
      if (maxHarmonic < 1)
        maxHarmonic = 1;
      for (int i = 0; i < kWavetableSize; ++i) {
        float t = float(i) / float(kWavetableSize);
        float sum = 0.0f;
        for (int n = 1; n <= maxHarmonic; ++n) {
          sum += std::sin(2.0f * float(M_PI) * n * t) / float(n);
        }
        float val = -2.0f * sum / float(M_PI);
        int ival = int(val * 32767.0f);
        if (ival > 32767)
          ival = 32767;
        if (ival < -32767)
          ival = -32767;
        runtimeTables[level][i] = (int16_t)ival;
      }
    }
  }
#endif

  static int selectLevel(uint32_t phaseInc) {
    if (phaseInc == 0)
      return 0;
    int log2val = 31 - __builtin_clz(phaseInc);
    if (phaseInc & (phaseInc - 1))
      log2val += 1;
    int level = log2val - 21;
    if (level < 0)
      level = 0;
    if (level >= kNumMipLevels)
      level = kNumMipLevels - 1;
    return level;
  }

  inline int32_t get(uint32_t phase, int level) const {
    int i0 = phase >> (32 - kWavetableBits);
    int i1 = (i0 + 1) & (kWavetableSize - 1);
    int frac = (phase >> (32 - kWavetableBits - 8)) & 0xFF;
#ifdef MIPSAW_USE_PRECOMPUTED_TABLES
    int32_t s0 = kMipSawTables[level][i0];
    int32_t s1 = kMipSawTables[level][i1];
#else
    int32_t s0 = runtimeTables[level][i0];
    int32_t s1 = runtimeTables[level][i1];
#endif
    return s0 + (((s1 - s0) * frac) >> 8);
  }

private:
#ifndef MIPSAW_USE_PRECOMPUTED_TABLES
  std::array<std::array<int16_t, kWavetableSize>, kNumMipLevels> runtimeTables;
#endif
};

// Dedicated float sine LUT for the additive partial bank (modal/sine).
// Faster than going through MipSawTable::get() because it skips the
// int16->float conversion and the * (1/32767) scale.
class SineTable {
public:
  static constexpr int kBits = 10;
  static constexpr int kSize = 1 << kBits;
  static constexpr int kFracBits = 32 - kBits;
  static constexpr float kInvFrac = 1.0f / (float)(1u << kFracBits);

  SineTable() {
    for (int i = 0; i <= kSize; ++i) {
      table[i] = std::sin(2.0f * float(M_PI) * float(i) / float(kSize));
    }
  }

  inline float get(uint32_t phase) const {
    uint32_t i = phase >> kFracBits;
    float frac = (float)(phase & ((1u << kFracBits) - 1)) * kInvFrac;
    float a = table[i];
    return a + (table[i + 1] - a) * frac;
  }

private:
  // +1 entry so linear interp at the last index needs no wrap.
  float table[kSize + 1];
};

static const SineTable gSineTable;

// State Variable Filter (SVF) - Chamberlin topology, 2x oversampled.
class SVFCutoffTable {
public:
  static constexpr int kTableSize = 1024;
  SVFCutoffTable() {
    for (int i = 0; i < kTableSize; ++i) {
      float c = float(i) / (kTableSize - 1);
      float f = 2.0f * std::sin(float(M_PI) * c * 0.5f);
      if (f > 1.0f)
        f = 1.0f;
      table[i] = f;
    }
  }
  float lookup(float cutoff) const {
    float idx = cutoff * (kTableSize - 1);
    int i0 = int(idx);
    if (i0 >= kTableSize - 1)
      return table[kTableSize - 1];
    int i1 = i0 + 1;
    float frac = idx - i0;
    return table[i0] * (1.0f - frac) + table[i1] * frac;
  }

private:
  std::array<float, kTableSize> table;
};

static const SVFCutoffTable gSVFCutoffTable;

class PWMLfoRateTable {
public:
  static constexpr int kTableSize = 1024;
  PWMLfoRateTable() {
    for (int i = 0; i < kTableSize; ++i) {
      float c = float(i) / (kTableSize - 1);
      table[i] = 0.05f * std::pow(500.0f, c);
    }
  }
  float lookup(float effect) const {
    float idx = effect * (kTableSize - 1);
    int i0 = int(idx);
    if (i0 >= kTableSize - 1)
      return table[kTableSize - 1];
    int i1 = i0 + 1;
    float frac = idx - i0;
    return table[i0] * (1.0f - frac) + table[i1] * frac;
  }

private:
  std::array<float, kTableSize> table;
};

static const PWMLfoRateTable gPWMLfoRateTable;

class SVFLowpass {
public:
  void setParams(float cutoff, float resonance) {
    f = gSVFCutoffTable.lookup(cutoff);
    q = 2.0f * (1.0f - resonance);
    if (q < 0.05f)
      q = 0.05f;
  }

  float process(float in) {
    float fh = f * 0.5f;
    lp += fh * bp;
    float hp = in - lp - q * bp;
    bp += fh * hp;
    lp += fh * bp;
    hp = in - lp - q * bp;
    bp += fh * hp;
    return lp;
  }

  // True when both state variables are small enough that further driven-by-
  // zero ringing is below audibility. Used by the voice-idle path to know
  // when it's safe to stop calling process() without truncating a tail.
  bool isQuiet(float thresh = 1e-5f) const {
    return std::abs(lp) < thresh && std::abs(bp) < thresh;
  }

  void reset() { lp = bp = 0.0f; }

private:
  float lp = 0.0f, bp = 0.0f, f = 0.0f, q = 0.0f;
};

// Multi-engine voice. Carries state for the dual-saw (subtractive) path,
// the additive partial bank (modal / sine), and the DPW pulse (PWM). Only
// the state for the active mode is touched in process(), so the other
// halves are dead memory but keep the struct simple and avoid per-mode
// allocation.
struct SynthVoice {
  bool active = false;
  int mode = SYNTH_SUBTRACTIVE;
  float baseFreq = 0.0f;
  float onvelocity = 0.0f;  // captured at note-on-velocity (0..1)
  float offvelocity = 0.0f; // captured at note-off-velocity (0..1)
  float pressure = 0.0f;    // continuous, updated via setVoicePressure (0..1)
  int triggerOrder = 0;     // for voice stealing

  // --- Subtractive (mode 0): dual saw with detune ---
  uint32_t phase1 = 0, phase2 = 0;
  uint32_t phaseInc1 = 0, phaseInc2 = 0;
  int mipLevel1 = 0, mipLevel2 = 0;

  // --- Additive bank (modes 1, 2): N sine partials ---
  // partialAmp follows partialTarget with one-pole speed = partialDecay.
  // For modal: target=0 with per-partial T60-derived decay → exp ringdown.
  // For sine bank:   target=current amp with slow decay (=smoothing)
  //                  → sustain that follows live KNOB_EFFECT changes
  //                  without zipper.
  uint32_t partialPhase[kPartials] = {};
  uint32_t partialInc[kPartials] = {};
  float partialAmp[kPartials] = {};
  float partialTarget[kPartials] = {};
  float partialDecay[kPartials] = {};

  // --- PWM (mode 3): DPW pulse + PWM LFO ---
  // Pulse synthesised as the difference of two DPW saws (one phase-shifted
  // by `duty`). dpwPrev* hold the polynomial value from the previous sample
  // for the discrete differentiation; dpwScale = fs/(4*f0) compensates the
  // diff so each saw branch is unit-amplitude.
  float dpwPrev1 = 0.0f;
  float dpwPrev2 = 0.0f;
  uint32_t pwmLfoPhase = 0;
  uint32_t pwmLfoInc = 0;
  float pwmCenter = 0.5f; // base duty (pressure-driven)
  float pwmDepth = 0.0f;  // LFO depth around base duty (pressure-driven)
  float dpwScale = 1.0f;

  // --- Saw Morph (mode 2) ---
  int mipLevel3 = 0;
  float sawMorphLerp = 0.0f;

  // --- Common ---
  SVFLowpass filter;
  float gain = 0.0f;
  // One-pole smoother applied to env*gain at the output stage. Coefficient
  // ~0.05 ⇒ ≈0.4 ms time constant — barely affects the audible attack but
  // removes the residual step at note-off threshold, voice steal, and
  // block-rate gain changes that would otherwise click.
  float ampSmooth = 0.0f;

  // Per-voice envelope: smooth attack at noteOn, mode-dependent release at
  // noteOff. For modal the partials carry the natural decay while the
  // envelope is just a fade to silence when the key lifts.
  float env = 0.0f;
  float envTarget = 0.0f;
  float envCoef = 0.02f;

  void noteOff() {
    envTarget = 0.0f;
    // Modal gets a long release so natural ring-down is audible after key
    // release. Other modes cut quickly to avoid stacking sustained notes.
    float releaseSec = (mode == SYNTH_MODAL) ? 0.4f : 0.05f;
    envCoef = 1.0f - std::exp(-6.9f / (releaseSec * gSampleRate));
    if (envCoef > 1.0f)
      envCoef = 1.0f;
  }

  bool isIdle() const {
    return !active && env < 1e-5f && ampSmooth < 1e-5f && filter.isQuiet();
  }
};

class SimpleSynth {
public:
  static void setSampleRate(float sr) {
    gSampleRate = sr;
    gPhaseScale = 4294967296.0f / sr;
  }

  const char *getModeName() const {
    switch (soundMode) {
    case SYNTH_SUBTRACTIVE:
      return "Subtractive";
    case SYNTH_MODAL:
      return "Modal";
    case SYNTH_SAW_MORPH:
      return "Saw Morph";
    case SYNTH_PWM:
      return "PWM Pulse";
    default:
      return "Unknown";
    }
  }

  // Called once per audio block with the 8 knob values (0..1 each).
  void setKnobs(const float *k) {
    knobVolume = k[KNOB_VOLUME];
    knobCutoff = k[KNOB_CUTOFF];
    knobResonance = k[KNOB_RESONANCE];
    knobEffect = k[KNOB_EFFECT];
    knobTouch = k[KNOB_TOUCH] * 2.0f - 1.0f;
    knobVelocity = k[KNOB_VELOCITY];
    knobCurve = k[KNOB_CURVE];

    // Precompute applyCurve constants for this block
    float cK = (0.5f - knobCurve) * 1.98f;
    mCurveNumScale = 1.0f - cK;
    mCurveDenOffset = 1.0f + cK;
    mCurveDenScale = -2.0f * cK;

    // KNOB_SOUND → preset 0..3
    int newMode = (int)(k[KNOB_SOUND] * 3.999f);
    if (newMode < 0)
      newMode = 0;
    if (newMode > 3)
      newMode = 3;
    soundMode = newMode;

    // KNOB_EFFECT also drives delay wet — single "more effect" knob feel.
    delay.setMix(knobEffect * 0.7f);

    for (int i = 0; i < kNumVoices; ++i) {
      if (voices[i].active)
        updateVoice(i);
    }
  }

  void noteOn(int voice, float freq) {
    if (voice < 0 || voice >= kNumVoices)
      return;
    SynthVoice &v = voices[voice];
    v.active = true;
    v.mode =
        soundMode; // mode locked at note-on; live changes only affect new notes
    v.baseFreq = freq;
    v.onvelocity = 0.0f;
    v.offvelocity = 0.0f;
    v.pressure = 0.0f;
    v.env = 0.0f;
    v.envTarget = 1.0f;
    v.envCoef = 0.02f; // ~2.5 ms attack at 48 kHz — smooth, no click
    v.filter.reset();
    initVoiceForMode(v);
    updateVoice(voice);
  }

  void noteOff(int voice) {
    if (voice < 0 || voice >= kNumVoices)
      return;
    voices[voice].noteOff();
  }

  void SetOnVelocityForVoice(int voice, float velocity) {
    if (voice >= 0 && voice < kNumVoices) {
      SynthVoice &v = voices[voice];
      v.onvelocity = velocity;
      if (v.active) {
        updateVoice(voice);
        // If modal, we perform the initial strike now that velocity is known.
        if (v.mode == SYNTH_MODAL) {
          float vCurved = applyCurve(v.onvelocity);
          float nyq = gSampleRate * 0.45f;
          for (int p = 0; p < kPartials; p++) {
            int n = p + 1;
            float effect = knobEffect;
            float stretch = 1.0f + 0.005f * effect * (float)(n * n);
            float pf = v.baseFreq * (float)n * stretch;
            float a = vCurved * 6.0f / (1.0f + 0.5f * (float)p);
            if (pf > nyq)
              a = 0.0f;
            v.partialAmp[p] = a;
          }
        }
      }
    }
  }
  void SetOffVelocityForVoice(int voice, float velocity) {
    if (voice >= 0 && voice < kNumVoices) {
      voices[voice].offvelocity = velocity;
      if (voices[voice].active)
        updateVoice(voice);
    }
  }

  void setVoicePressure(int voice, float pressure) {
    if (voice >= 0 && voice < kNumVoices) {
      voices[voice].pressure = pressure;
      if (voices[voice].active)
        updateVoice(voice);
    }
  }

  // ── Block-rendering API for fork-join across cores ───────────────────────
  //
  // Workflow expected for a multicore audio block:
  //   core1: prepareBlock()                       // smoother once
  //   core1 → core0 fork
  //   core0: renderVoiceRangeAdd(scratch0, N, 8, 12)
  //   core1: renderVoiceRangeAdd(scratch1, N, 0,  8)   // in parallel
  //   core1: join, sum scratches into one float buffer
  //   core1: finalizeBlockI2S(summed, outI2S, N)       // delay + I2S words

  // Run the partial-amp smoother once for the upcoming block.
  inline void prepareBlock() { runPartialSmoother(); }

  // Add `numFrames` samples of voices [first, last) into out[]. Caller must
  // pre-clear out[] before the first range. Reads voice state only — safe
  // to run on a different core than the one that mutates voices, provided
  // setKnobs/noteOn don't run while this is in flight.
  inline void renderVoiceRangeAdd(float *out, size_t numFrames, int first,
                                  int last) {
    if (first < 0)
      first = 0;
    if (last > kNumVoices)
      last = kNumVoices;

    for (int idx = first; idx < last; idx++) {
      SynthVoice &v = voices[idx];
      if (v.isIdle()) {
        v.filter.reset();
        v.ampSmooth = 0.0f;
        continue;
      }

      if (v.mode == SYNTH_SUBTRACTIVE) {
        for (size_t i = 0; i < numFrames; i++) {
          out[i] += processSubtractiveSample(v);
        }
      } else if (v.mode == SYNTH_PWM) {
        for (size_t i = 0; i < numFrames; i++) {
          out[i] += processPWMSample(v);
        }
      } else if (v.mode == SYNTH_SAW_MORPH) {
        for (size_t i = 0; i < numFrames; i++) {
          out[i] += processSawMorphSample(v);
        }
      } else {
        // Modal / Sine Bank
        for (size_t i = 0; i < numFrames; i++) {
          out[i] += processAdditiveSample(v);
        }
      }
    }
  }

  // Take a pre-summed voice buffer, run the stereo delay, and write the
  // I2S-ready word stream [R0, L0, R1, L1, ...] expected by process_audio.
  inline void finalizeBlockI2S(const float *summed, int32_t *outI2S,
                               size_t numFrames) {
    constexpr float kSumToInt16 = 32767.0f * kInvNumVoices;
    for (size_t i = 0; i < numFrames; i++) {
      int32_t L = (int32_t)(summed[i] * kSumToInt16);
      int32_t R = L;
      delay.process(L, R);
      outI2S[i * 2] = (int16_t)R << 16;
      outI2S[i * 2 + 1] = (int16_t)L << 16;
    }
  }

  int allocateVoice() {
    for (int i = 0; i < kNumVoices; ++i) {
      if (!voices[i].active) {
        return i;
      }
    }
    int oldest = 0;
    int minOrder = voices[0].triggerOrder;
    for (int i = 1; i < kNumVoices; ++i) {
      if (voices[i].triggerOrder < minOrder) {
        minOrder = voices[i].triggerOrder;
        oldest = i;
      }
    }
    return oldest;
  }

  void updateVoiceTriggerOrder(int voice) {
    static int globalTriggerCounter = 0;
    if (voice >= 0 && voice < kNumVoices) {
      voices[voice].triggerOrder = ++globalTriggerCounter;
    }
  }

  std::array<SynthVoice, kNumVoices> voices;

  // Apply the curve knob to a 0..1 control source using a rational sigmoid.
  // knobCurve=0.5 → linear; knobCurve=0 → concave (hard touch);
  // knobCurve=1 → convex (light touch).
  inline float applyCurve(float x) const {
    if (x <= 0.0f)
      return 0.0f;
    if (x >= 1.0f)
      return 1.0f;

    return (x * mCurveNumScale) / (mCurveDenOffset + mCurveDenScale * x);
  }

private:
  // Initialise per-mode state at note-on.
  void initVoiceForMode(SynthVoice &v) {
    float fScale = gPhaseScale;
    float vCurved = v.onvelocity;
    float fs = gSampleRate;
    float nyq = fs * 0.45f;

    if (v.mode == SYNTH_SUBTRACTIVE) {
      v.phase1 = 0;
      v.phaseInc1 = (uint32_t)(v.baseFreq * fScale);
      v.mipLevel1 = MipSawTable::selectLevel(v.phaseInc1);
      v.phase2 = 0;
      v.phaseInc2 = v.phaseInc1;
      v.mipLevel2 = v.mipLevel1;
      return;
    }

    if (v.mode == SYNTH_PWM) {
      v.phase1 = 0;
      v.phaseInc1 = (uint32_t)(v.baseFreq * fScale);
      v.phaseInc2 = v.phaseInc1; // both saw branches at the same pitch
      // dpwScale = T/4 (in samples) compensates the discrete differentiation
      // so each saw branch has unit amplitude.
      v.dpwScale = fs / (4.0f * v.baseFreq);
      v.pwmLfoPhase = 0;
      // Seed the diff state to the polynomial values at phase=0 (p1=0) and
      // phase=0.5 (p2 = (-1)² = 1). Means the very first sample produces a
      // saw step of ~0 instead of a giant jump. Real values get filled in
      // immediately by updateVoice() (called right after).
      v.dpwPrev1 = 0.0f;
      v.dpwPrev2 = 1.0f;
      return;
    }

    if (v.mode == SYNTH_SAW_MORPH) {
      v.phase1 = 0;
      v.phaseInc1 = (uint32_t)(v.baseFreq * fScale);
      v.mipLevel1 = MipSawTable::selectLevel(v.phaseInc1);
      v.phase2 = 0;
      v.phaseInc2 = v.phaseInc1;
      v.mipLevel2 = v.mipLevel1;
      v.mipLevel3 = v.mipLevel1;
      return;
    }

    for (int p = 0; p < kPartials; p++)
      v.partialPhase[p] = 0;

    if (v.mode == SYNTH_MODAL) {
      // Inharmonic damped sines. KNOB_EFFECT lengthens decay and bends the
      // partial spacing toward a bell-like spectrum.
      float effect = knobEffect;
      float t60Base = 0.3f + effect * 4.0f; // 0.3 .. 4.3 s
      for (int p = 0; p < kPartials; p++) {
        int n = p + 1;
        float stretch = 1.0f + 0.005f * effect * (float)(n * n);
        float pf = v.baseFreq * (float)n * stretch;
        v.partialInc[p] = (uint32_t)(pf * fScale);
        float a =
            vCurved * 6.0f /
            (1.0f + 0.5f * (float)p); // Boosted further to match Subtractive
        if (pf > nyq)
          a = 0.0f;
        v.partialAmp[p] = a;
        v.partialTarget[p] = 0.0f;
        float t60 = t60Base / (1.0f + 0.7f * (float)p);
        // Decay raised to kSmootherBlockSize because the smoother runs at
        // block rate, not per-sample. Same absolute T60.
        v.partialDecay[p] =
            std::exp(-6.9f * (float)kSmootherBlockSize / (t60 * fs));
      }
    }
  }

  // Recompute live-tweakable per-voice params (cutoff, gain, mode-specific FX).
  // Called from setKnobs() and setVoicePressure() — knob changes apply to all
  // active voices, pressure changes apply per-voice.
  void updateVoice(int idx) {
    SynthVoice &v = voices[idx];

    float pCurved = applyCurve(v.pressure);
    float vCurvedAtAttack = v.onvelocity;

    // Volume: master × (1 + (vel-1)*velocityKnob) — at velocityKnob=0 velocity
    // has no effect; at 1 it scales linearly with velocity.
    float velScale = 1.0f + (vCurvedAtAttack - 1.0f) * knobVelocity;
    if (velScale < 0.0f)
      velScale = 0.0f;
    v.gain = 0.5f * knobVolume * velScale;

    // Cutoff: base + pressure*touch (positive only).
    float cutoff = knobCutoff + pCurved * knobTouch;
    if (cutoff > 1.0f)
      cutoff = 1.0f;
    if (cutoff < 0.0f)
      cutoff = 0.0f;
    cutoff *= cutoff; // perceptual exponential-ish shaping
    v.filter.setParams(cutoff, knobResonance);

    // Per-mode live FX response:
    if (v.mode == SYNTH_SUBTRACTIVE) {
      // KNOB_EFFECT = detune amount (0..1 → 0..0.4 semitones)
      float ratio = 1.0f + knobEffect * 0.4f * 0.057762265f;
      float freq2 = v.baseFreq * ratio;
      v.phaseInc2 = (uint32_t)(freq2 * gPhaseScale);
      v.mipLevel2 = MipSawTable::selectLevel(v.phaseInc2);
    } else if (v.mode == SYNTH_SAW_MORPH) {
      // 3-saw morph: base saw + crossfade between harmonics floor(N) and
      // floor(N)+1 N goes from 2 to 7 based on pressure.
      float N = 2.0f + pCurved * 5.0f;
      float fN = (float)((int)N); // Faster than std::floor on positive numbers
      v.sawMorphLerp = N - fN;

      v.phaseInc1 = (uint32_t)(v.baseFreq * gPhaseScale);
      v.mipLevel1 = MipSawTable::selectLevel(v.phaseInc1);

      v.phaseInc2 = (uint32_t)(v.baseFreq * fN * gPhaseScale);
      v.mipLevel2 = MipSawTable::selectLevel(v.phaseInc2);
      v.mipLevel3 = MipSawTable::selectLevel(v.phaseInc2 * 2);
    } else if (v.mode == SYNTH_MODAL) {
      // Pressure adds energy to the modal partials by setting a non-zero
      // target. This allows the sound to sustain and swell with pressure, like
      // a bowed bell.
      float pCurved = applyCurve(v.pressure);
      for (int p = 0; p < kPartials; p++) {
        float baseA =
            6.0f /
            (1.0f + 0.5f * (float)p); // Boosted further to match Subtractive
        v.partialTarget[p] = pCurved * std::abs(knobTouch) * baseA;
      }
    } else if (v.mode == SYNTH_PWM) {
      // Pressure → base pulse width and PWM depth.
      //   pCurved=0 : narrow static pulse (~5%), no LFO mod
      //   pCurved=1 : square (~50%), strong PWM swing (±40%)
      v.pwmCenter = 0.05f + 0.45f * pCurved;
      v.pwmDepth = 0.4f * pCurved;
      // KNOB_EFFECT → LFO rate, exponential 0.05 Hz .. ~25 Hz.
      float lfoHz = gPWMLfoRateTable.lookup(knobEffect);
      v.pwmLfoInc = (uint32_t)(lfoHz * gPhaseScale);
    }
  }

  inline float processSubtractiveSample(SynthVoice &v) {
    if (!v.active && v.env < 1e-5f && v.ampSmooth < 1e-5f)
      return v.filter.process(0.0f);

    v.env += v.envCoef * (v.envTarget - v.env);
    if (v.envTarget == 0.0f && v.env < 0.0005f) {
      v.env = 0.0f;
      v.active = false;
    }

    int32_t o1 = wavetable.get(v.phase1, v.mipLevel1);
    int32_t o2 = wavetable.get(v.phase2, v.mipLevel2);
    constexpr float kInv = 1.0f / 32767.0f;
    float out = (float)(o1 + o2) * (0.5f * kInv);
    v.phase1 += v.phaseInc1;
    v.phase2 += v.phaseInc2;

    float ampTarget = v.env * v.gain;
    v.ampSmooth += 0.05f * (ampTarget - v.ampSmooth);
    out *= v.ampSmooth;
    return v.filter.process(out);
  }

  inline float processPWMSample(SynthVoice &v) {
    if (!v.active && v.env < 1e-5f && v.ampSmooth < 1e-5f)
      return v.filter.process(0.0f);

    v.env += v.envCoef * (v.envTarget - v.env);
    if (v.envTarget == 0.0f && v.env < 0.0005f) {
      v.env = 0.0f;
      v.active = false;
    }

    float lfo = gSineTable.get(v.pwmLfoPhase);
    v.pwmLfoPhase += v.pwmLfoInc;

    float duty = v.pwmCenter + v.pwmDepth * lfo;
    if (duty < 0.02f)
      duty = 0.02f;
    if (duty > 0.98f)
      duty = 0.98f;

    uint32_t dutyOffset = (uint32_t)(duty * 4294967296.0f);
    uint32_t ph2u = v.phase1 + dutyOffset;

    constexpr float kS = 1.0f / 2147483648.0f;
    float ph1f = (float)((int32_t)v.phase1) * kS;
    float ph2f = (float)((int32_t)ph2u) * kS;

    float p1 = ph1f * ph1f;
    float p2 = ph2f * ph2f;

    float saw1 = (p1 - v.dpwPrev1) * v.dpwScale;
    float saw2 = (p2 - v.dpwPrev2) * v.dpwScale;
    v.dpwPrev1 = p1;
    v.dpwPrev2 = p2;

    float out = (saw1 - saw2) + (1.0f - 2.0f * duty);
    v.phase1 += v.phaseInc1;

    float ampTarget = v.env * v.gain;
    v.ampSmooth += 0.05f * (ampTarget - v.ampSmooth);
    out *= v.ampSmooth;
    return v.filter.process(out);
  }

  inline float processSawMorphSample(SynthVoice &v) {
    if (!v.active && v.env < 1e-5f && v.ampSmooth < 1e-5f)
      return v.filter.process(0.0f);

    v.env += v.envCoef * (v.envTarget - v.env);
    if (v.envTarget == 0.0f && v.env < 0.0005f) {
      v.env = 0.0f;
      v.active = false;
    }

    int32_t s1 = wavetable.get(v.phase1, v.mipLevel1);
    int32_t s2 = wavetable.get(v.phase2, v.mipLevel2);
    int32_t s3 = wavetable.get(v.phase2 * 2, v.mipLevel3);

    constexpr float kInv = 1.0f / 32767.0f;
    float fs1 = (float)s1 * kInv;
    float fs2 = (float)s2 * kInv;
    float fs3 = (float)s3 * kInv;

    // output = 1st + lerp(2nd, 3rd, fmod(N,1.0f))
    float morph = fs2 + (fs3 - fs2) * (1.0f - v.sawMorphLerp);
    float out = (fs1 + morph) * 0.5f;

    v.phase1 += v.phaseInc1;
    v.phase2 += v.phaseInc2;

    float ampTarget = v.env * v.gain;
    v.ampSmooth += 0.05f * (ampTarget - v.ampSmooth);
    out *= v.ampSmooth;
    return v.filter.process(out);
  }

  inline float processAdditiveSample(SynthVoice &v) {
    if (!v.active && v.env < 1e-5f && v.ampSmooth < 1e-5f)
      return v.filter.process(0.0f);

    v.env += v.envCoef * (v.envTarget - v.env);
    if (v.envTarget == 0.0f && v.env < 0.0005f) {
      v.env = 0.0f;
      v.active = false;
    }

    constexpr float kPartialScale = 1.0f / (float)kPartials;
    float sum = 0.0f;
    for (int p = 0; p < kPartials; p++) {
      float amp = v.partialAmp[p];
      if (amp < kPartialDeadThreshold && v.partialTarget[p] == 0.0f) {
        v.partialAmp[p] = 0.0f;
        continue;
      }
      sum += gSineTable.get(v.partialPhase[p]) * amp;
      v.partialPhase[p] += v.partialInc[p];
    }
    float out = sum * kPartialScale;

    float ampTarget = v.env * v.gain;
    v.ampSmooth += 0.05f * (ampTarget - v.ampSmooth);
    out *= v.ampSmooth;
    return v.filter.process(out);
  }

  // One pass of the partial-amp smoother. Block-rate operation. partialDecay
  // coefficients in SynthVoice are already raised to kSmootherBlockSize so
  // the time constant matches the original per-sample formulation.
  inline void runPartialSmoother() {
    for (auto &v : voices) {
      if (!v.active && v.env < 0.0005f)
        continue;
      if (v.mode == SYNTH_SUBTRACTIVE || v.mode == SYNTH_PWM)
        continue;
      for (int p = 0; p < kPartials; p++) {
        float t = v.partialTarget[p];
        v.partialAmp[p] = t + (v.partialAmp[p] - t) * v.partialDecay[p];
      }
    }
  }

  MipSawTable wavetable;
  StereoDelay delay;

  int soundMode = 0;
  float knobVolume = 1.0f;
  float knobCutoff = 0.5f;
  float knobResonance = 0.0f;
  float knobEffect = 0.0f;
  float knobTouch = 0.5f;
  float knobVelocity = 0.5f;
  float knobCurve = 0.5f;

  float mCurveNumScale = 1.0f;
  float mCurveDenOffset = 1.0f;
  float mCurveDenScale = 0.0f;
};
