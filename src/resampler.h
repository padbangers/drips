// Streaming sample-rate converter between the MPC's 44.1 kHz and the rate Beads runs at in the selected quality
// (48, 32 or 24 kHz): band-limited interpolation with a Kaiser-windowed sinc (8 zero crossings per side), cut off at
// 0.95 of the lower rate's Nyquist frequency. The ratio is exact (integer rates), so a down- and an up-converter
// with swapped rates produce, over time, exactly as many frames as went in.
//
// Polyphase: an output falls on one of out_rate / gcd(in_rate, out_rate) fractional positions between two inputs
// (at most 441, for 32 -> 44.1 kHz), and Init() computes the kernel for each, so Process() is a plain
// multiply-accumulate over a contiguous stretch of the input history. Init() is slow (a few ms): never call it from
// the audio thread.
#pragma once

#include <math.h>
#include <stddef.h>
#include <string.h>

namespace drips {

struct StereoFrame {
  float l, r;
};

class Resampler {
 public:
  void Init(int in_rate, int out_rate) {
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    int a = in_rate, b = out_rate;
    while (b) { int t = a % b; a = b; b = t; }
    step_ = a;                                   // frac_ only takes multiples of the gcd
    phases_ = out_rate / step_;
    float lower = static_cast<float>(in_rate < out_rate ? in_rate : out_rate);
    float cutoff = 0.95f * lower / static_cast<float>(in_rate);   // of the input's Nyquist frequency
    half_width_ = static_cast<int>(ceilf(kZeroCrossings / cutoff)) + 1;
    taps_ = 2 * half_width_;
    if (phases_ > kMaxPhases || taps_ > kMaxTaps) {   // not for the rates used here; pass silence rather than overrun
      phases_ = 1;
      taps_ = 0;
    }
    memset(history_, 0, sizeof history_);
    written_ = 0;
    pos_ = 0;
    frac_ = 0;
    // kernel[phase][j] weighs input pos_ - half_width_ + 1 + j for an output phase * step_ / out_rate past pos_
    const double beta = 7.0;
    const double i0_beta = BesselI0(beta);
    for (int p = 0; p < phases_; ++p) {
      double t = static_cast<double>(p) * step_ / out_rate;
      for (int j = 0; j < taps_; ++j) {
        double u = fabs((t - (j - half_width_ + 1)) * cutoff);
        double w = 0.0;
        if (u < kZeroCrossings) {
          double sinc = u < 1e-9 ? 1.0 : sin(M_PI * u) / (M_PI * u);
          double r = u / kZeroCrossings;
          w = cutoff * sinc * BesselI0(beta * sqrt(1.0 - r * r)) / i0_beta;
        }
        kernel_[p * kMaxTaps + j] = static_cast<float>(w);
      }
    }
  }

  // Feeds n input frames; writes the output frames they complete to out and returns how many. out needs room for
  // n * out_rate / in_rate + 2 frames.
  size_t Process(const StereoFrame* in, size_t n, StereoFrame* out) {
    size_t produced = 0;
    for (size_t i = 0; i < n; ++i) {
      // every input twice, kRing apart, so a kernel's stretch of history is always contiguous
      int w = static_cast<int>(written_ & kMask);
      history_[w] = history_[w + kRing] = in[i];
      ++written_;
      // the output at input time pos_ + frac_ / out_rate_ needs inputs up to pos_ + half_width_
      while (pos_ + half_width_ < written_) {
        const float* k = &kernel_[(frac_ / step_) * kMaxTaps];
        const StereoFrame* x = &history_[(pos_ - half_width_ + 1) & kMask];
        float l = 0.0f, r = 0.0f;
        for (int j = 0; j < taps_; ++j) {
          l += k[j] * x[j].l;
          r += k[j] * x[j].r;
        }
        out[produced].l = l;
        out[produced].r = r;
        ++produced;
        frac_ += in_rate_;
        pos_ += frac_ / out_rate_;
        frac_ %= out_rate_;
      }
    }
    return produced;
  }

 private:
  static const int kZeroCrossings = 8;
  static const int kMaxPhases = 441;   // 32 -> 44.1 kHz
  static const int kMaxTaps = 36;      // 2 * (ceil(8 / cutoff) + 1); cutoff >= 0.95 * 24 / 44.1
  static const int kRing = 64;         // > taps
  static const int kMask = kRing - 1;

  static double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
      term *= (x / (2.0 * k)) * (x / (2.0 * k));
      sum += term;
    }
    return sum;
  }

  int in_rate_, out_rate_;
  int step_, phases_;
  int half_width_, taps_;
  StereoFrame history_[2 * kRing];
  long long written_;   // input frames so far
  long long pos_;       // integer part of the next output's time, in input frames
  int frac_;            // its fractional part, in 1 / out_rate_ steps
  float kernel_[kMaxPhases * kMaxTaps];
};

}  // namespace drips
