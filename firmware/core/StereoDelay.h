#pragma once
// The synth's effect: a stereo delay. Two fixed delay lines feed each other, so the repeats bounce between the sides.
#include <stdint.h>
#include <string.h>

class StereoDelay {
public:
  // Cleared here rather than with initialisers, which would put 56 KB of zeros in the firmware image.
  StereoDelay() {
    memset(left.samples, 0, sizeof left.samples);
    memset(right.samples, 0, sizeof right.samples);
  }

  // 0 is dry, 1 is only the repeats.
  void setMix(float mix) {
    wet = mix <= 0.0f ? 0 : mix >= 1.0f ? 256 : (int32_t)(mix * 256.0f);
  }

  void process(int32_t &L, int32_t &R) {
    const int32_t repeatL = left.read(), repeatR = right.read();
    left.write(clip(L + feedback(repeatR, driftR)));
    right.write(clip(R + feedback(repeatL, driftL)));
    L = (L * (256 - wet) + repeatL * wet) >> 8;
    R = (R * (256 - wet) + repeatR * wet) >> 8;
  }

private:
  static constexpr int32_t kFeedback = 43690; // two thirds, of 65536

  // Each slot is read just before it is written, so a line of N samples delays by N.
  template <int N> struct Line {
    int16_t samples[N];
    int pos = 0;
    int32_t read() const { return samples[pos]; }
    void write(int32_t sample) {
      samples[pos] = (int16_t)sample;
      if (++pos == N)
        pos = 0;
    }
  };

  // The repeat, turned down, without the slow drift that would otherwise build up over the passes.
  static int32_t feedback(int32_t repeat, int32_t &drift) {
    const int32_t sample = (repeat * kFeedback) >> 16;
    drift += (sample - drift) >> 8;
    return sample - drift;
  }

  static int32_t clip(int32_t sample) { return sample > 32767 ? 32767 : sample < -32767 ? -32767 : sample; }

  Line<12288> left;  // about 250 ms at the codec's rate
  Line<16384> right; // about 335 ms
  int32_t driftL = 0, driftR = 0;
  int32_t wet = 0;
};
