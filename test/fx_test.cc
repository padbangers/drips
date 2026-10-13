// Offline check of the effect: feeds signals through process() in every quality, source and buffer setting and
// reports output level, clipping, delay mode, freeze, the Seed input (button, MIDI, clock), the state chunk and the
// time per 128-frame block (native, so only a relative number; the device figure comes from tools/bench.sh).
//   make -C test run

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "engine.h"
}

namespace {

const int kBlock = 128;
const float kSampleRate = 44100.0f;
const char* kQualities[] = { "Bright Digital", "Cold Digital", "Sunny Tape", "Scorched Cassette" };

int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    printf("  FAIL: %s\n", what);
  }
}

double NowUs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

struct Result {
  double rms_signal, rms_tail, max_us, avg_us;
  int clipped, nans;
};

enum Input { NOISE_BURSTS, SINE, SILENCE };

typedef void (*BlockHook)(const mpc_engine_t* e, void* inst, int block);

// seconds of input (noise bursts 100 ms on / 100 ms off, or a 441 Hz sine), then the same time of silence
Result Run(const mpc_engine_t* e, void* inst, Input input, float seconds, BlockHook hook = NULL) {
  int16_t in[kBlock * 2], out[kBlock * 2];
  Result r = { 0, 0, 0, 0, 0, 0 };
  double e_sig = 0, e_tail = 0, total_us = 0;
  int n_sig = 0, n_tail = 0;
  int blocks = static_cast<int>(2.0f * seconds * kSampleRate / kBlock);
  unsigned seed = 1;
  for (int b = 0; b < blocks; ++b) {
    bool signal_part = b < blocks / 2;
    for (int i = 0; i < kBlock; ++i) {
      int frame = b * kBlock + i;
      int16_t v = 0;
      if (signal_part && input == NOISE_BURSTS && (frame / 4410) % 2 == 0) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<int16_t>((static_cast<int>(seed >> 16) - 32768) / 4);
      } else if (signal_part && input == SINE) {
        v = static_cast<int16_t>(16384.0 * sin(2.0 * M_PI * 441.0 * frame / kSampleRate));
      }
      in[i * 2] = v;
      in[i * 2 + 1] = v;
    }
    if (hook) hook(e, inst, b);
    double t0 = NowUs();
    e->process(inst, in, out, kBlock);
    double us = NowUs() - t0;
    total_us += us;
    if (us > r.max_us && b > 10) r.max_us = us;
    for (int i = 0; i < kBlock * 2; ++i) {
      double x = out[i] / 32768.0;
      if (x != x) ++r.nans;
      if (out[i] >= 32767 || out[i] <= -32768) ++r.clipped;
      if (signal_part) { e_sig += x * x; ++n_sig; } else { e_tail += x * x; ++n_tail; }
    }
  }
  r.rms_signal = sqrt(e_sig / n_sig);
  r.rms_tail = sqrt(e_tail / n_tail);
  r.avg_us = total_us / blocks;
  return r;
}

void Set(const mpc_engine_t* e, void* inst, const char* key, float v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%g", v);
  e->set_param(inst, key, buf);
}

void Print(const char* label, const Result& r) {
  printf("  %-44s rms in %.3f  tail %.3f  clipped %d  avg %.0f us  max %.0f us\n", label, r.rms_signal, r.rms_tail,
         r.clipped, r.avg_us, r.max_us);
}

// Runs silent blocks; a quality/buffer/source change runs on the engine's worker thread, so each block waits for it
// ("busy"), as the real-time host effectively does.
void Settle(const mpc_engine_t* e, void* inst, float seconds) {
  int16_t zero[kBlock * 2] = { 0 }, out[kBlock * 2];
  char busy[8];
  for (int i = 0; i < static_cast<int>(seconds * kSampleRate / kBlock); ++i) {
    while (e->get_param(inst, "busy", busy, sizeof busy) > 0 && busy[0] == '1') usleep(500);
    e->process(inst, zero, out, kBlock);
  }
}

void* Fresh(const mpc_engine_t* e, int quality, int source, int buffer) {
  void* inst = e->create(NULL);
  Set(e, inst, "quality", quality);
  Set(e, inst, "source", source);
  Set(e, inst, "buffer", buffer);
  Settle(e, inst, 0.6f);   // fade out, re-initialise, fade in
  return inst;
}

// Dry path through the converters: a sine fully dry comes out at the module's dry gain (0.75), same frequency.
void TestDryPath(const mpc_engine_t* e) {
  printf("dry path (441 Hz sine, dry/wet 0):\n");
  for (int q = 0; q < 4; ++q) {
    void* inst = Fresh(e, q, 0, 0);
    Set(e, inst, "dry_wet", 0);
    Set(e, inst, "density", 0.5f);
    Result r = Run(e, inst, SINE, 1.0f);
    char label[64];
    snprintf(label, sizeof label, "%s", kQualities[q]);
    Print(label, r);
    // input rms 0.5/sqrt(2) = 0.354; dry gain 0.75 -> 0.265 (Scorched Cassette's mu-law dry path, close to it).
    // The "tail" is the ~1 ms latency spilling past the end of the signal.
    Check(r.rms_signal > 0.2 && r.rms_signal < 0.32, "dry level");
    Check(r.nans == 0, "NaN");
    e->destroy(inst);
  }
}

void TestQualities(const mpc_engine_t* e) {
  printf("granular (noise bursts, dry/wet 100 %%, reverb 50 %%):\n");
  for (int buffer = 0; buffer < 2; ++buffer) {
    for (int q = 0; q < 4; ++q) {
      void* inst = Fresh(e, q, 0, buffer);
      Set(e, inst, "dry_wet", 1);
      Set(e, inst, "reverb", 0.5f);
      Set(e, inst, "density", 0.2f);
      Set(e, inst, "shape_amt", -50);
      Set(e, inst, "pitch_amt", 30);
      Result r = Run(e, inst, NOISE_BURSTS, 1.5f);
      char label[64];
      snprintf(label, sizeof label, "%s, %s", kQualities[q], buffer ? "mono" : "stereo");
      Print(label, r);
      Check(r.rms_signal > 0.01, "granular output");
      Check(r.nans == 0, "NaN");
      e->destroy(inst);
    }
  }
}

void TestDelay(const mpc_engine_t* e) {
  printf("delay mode (size fully clockwise, feedback 60 %%):\n");
  for (int q = 0; q < 4; ++q) {
    void* inst = Fresh(e, q, 0, 0);
    Set(e, inst, "size", 1);
    Set(e, inst, "density", 0.75f);
    Set(e, inst, "feedback", 0.6f);
    Set(e, inst, "dry_wet", 0.5f);
    Settle(e, inst, 1.6f);   // the module crossfades into delay mode over 1.5 s
    Result r = Run(e, inst, NOISE_BURSTS, 1.0f);
    char label[64];
    snprintf(label, sizeof label, "%s", kQualities[q]);
    Print(label, r);
    Check(r.rms_tail > 0.002, "delay repeats after the input stops");
    e->destroy(inst);
  }
}

void TestFreeze(const mpc_engine_t* e) {
  printf("freeze (on half way through the noise):\n");
  void* inst = Fresh(e, 0, 0, 0);
  Set(e, inst, "dry_wet", 1);
  Set(e, inst, "density", 0.3f);
  Result r = Run(e, inst, NOISE_BURSTS, 1.5f, [](const mpc_engine_t* e, void* inst, int b) {
    if (b == 250) Set(e, inst, "freeze", 1);
  });
  Print("Bright Digital, frozen", r);
  Check(r.rms_tail > 0.01, "frozen buffer keeps playing after the input stops");
  // a quality change while frozen must wait (the module ignores it too)
  Set(e, inst, "quality", 2);
  char buf[16];
  e->get_param(inst, "quality", buf, sizeof buf);
  Result r2 = Run(e, inst, SILENCE, 0.5f);
  Print("quality change while frozen (deferred)", r2);
  Check(r2.rms_signal > 0.01, "frozen buffer survives a quality change");
  e->destroy(inst);
}

void TestSeed(const mpc_engine_t* e) {
  printf("seed (density 12 o'clock: grains only from Seed):\n");
  // gated, Seed button taps
  void* inst = Fresh(e, 0, 0, 0);
  Set(e, inst, "dry_wet", 1);
  Set(e, inst, "density", 0.5f);
  Set(e, inst, "seed_mode", 1);
  Result quiet = Run(e, inst, NOISE_BURSTS, 1.0f);
  Print("gated, no taps", quiet);
  Result taps = Run(e, inst, NOISE_BURSTS, 1.0f, [](const mpc_engine_t* e, void* inst, int b) {
    if (b % 40 == 5) Set(e, inst, "seed", 1);
    if (b % 40 == 6) Set(e, inst, "seed", 0);
  });
  Print("gated, Seed taps", taps);
  Check(taps.rms_signal > 2 * quiet.rms_signal + 0.005, "Seed taps play grains");
  e->destroy(inst);

  // latched + MIDI notes on the Seed input
  inst = Fresh(e, 0, 0, 0);
  Set(e, inst, "dry_wet", 1);
  Set(e, inst, "density", 0.2f);   // clocked: divider; MIDI notes are the clock
  Set(e, inst, "seed_input", 1);
  Result midi = Run(e, inst, NOISE_BURSTS, 1.0f, [](const mpc_engine_t* e, void* inst, int b) {
    uint8_t on[3] = { 0x90, 67, 100 }, off[3] = { 0x80, 67, 0 };
    if (b % 30 == 3) e->midi(inst, on, 3);
    if (b % 30 == 10) e->midi(inst, off, 3);
  });
  Print("latched, MIDI notes as Seed clock", midi);
  Check(midi.rms_signal > 0.01, "MIDI notes clock grains");
  e->destroy(inst);

  // MPC clock 1/16 at 120 BPM, delay mode: tempo-synced delay
  inst = Fresh(e, 0, 0, 0);
  Set(e, inst, "size", 1);
  Set(e, inst, "seed_input", 6);
  Set(e, inst, "feedback", 0.85f);
  e->set_param(inst, "host_transport", "0 -1 120");
  Settle(e, inst, 1.6f);
  Result clock = Run(e, inst, NOISE_BURSTS, 1.0f);
  Print("delay clocked by the MPC tempo (1/16)", clock);
  Check(clock.rms_tail > 0.002, "clocked delay repeats");
  e->destroy(inst);
}

// MIDI Split 1/2: channel 1 transposes only, channel 2 seeds only. A frozen 441 Hz sine, clocked by MIDI (divider 1).
// Goertzel: signal power at f (left channel)
double Power(const int16_t* x, int frames, double f) {
  double c = 2 * cos(2 * M_PI * f / kSampleRate), s1 = 0, s2 = 0;
  for (int i = 0; i < frames; ++i) {
    double s0 = x[2 * i] / 32768.0 + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}

void TestMidiSplit(const mpc_engine_t* e) {
  printf("MIDI split (ch 1 pitch, ch 2 seed, frozen sine):\n");
  void* inst = Fresh(e, 0, 0, 0);
  Set(e, inst, "dry_wet", 1);
  Set(e, inst, "density", 0.1f);
  Set(e, inst, "seed_input", 1);
  Set(e, inst, "midi_split", 1);
  static int16_t out[kBlock * 2 * 400];
  int16_t in[kBlock * 2], tmp[kBlock * 2];
  for (int b = 0; b < 300; ++b) {   // record the sine, then freeze
    for (int i = 0; i < kBlock; ++i) {
      in[2 * i] = in[2 * i + 1] = static_cast<int16_t>(12000 * sin(2 * M_PI * 441.0 * (b * kBlock + i) / kSampleRate));
    }
    if (b == 290) Set(e, inst, "freeze", 1);
    e->process(inst, in, tmp, kBlock);
  }
  memset(in, 0, sizeof in);
  uint8_t pitch_on[3] = { 0x90, 72, 100 };        // channel 1: +12
  e->midi(inst, pitch_on, 3);
  double level[2];
  for (int phase = 0; phase < 2; ++phase) {        // 0: no channel 2 notes, 1: channel 2 notes every 60 ms
    for (int b = 0; b < 400; ++b) {
      if (phase == 1 && b % 20 == 0) { uint8_t on[3] = { 0x91, 40, 100 }; e->midi(inst, on, 3); }
      if (phase == 1 && b % 20 == 10) { uint8_t off[3] = { 0x81, 40, 0 }; e->midi(inst, off, 3); }
      e->process(inst, in, out + b * kBlock * 2, kBlock);
    }
    double acc = 0;
    for (int i = 0; i < 400 * kBlock * 2; ++i) acc += (out[i] / 32768.0) * (out[i] / 32768.0);
    level[phase] = sqrt(acc / (400 * kBlock * 2));
    printf("  %-44s rms %.3f\n", phase ? "channel 2 notes (note 40)" : "channel 1 note only (+12)", level[phase]);
  }
  Check(level[0] < 0.005, "channel 1 doesn't seed");
  Check(level[1] > 0.02, "channel 2 seeds");
  double p441 = Power(out, 400 * kBlock, 441.0), p882 = Power(out, 400 * kBlock, 882.0);
  printf("  power at 441 Hz %.1f, at 882 Hz %.1f\n", p441, p882);
  Check(p882 > 100 * p441, "grains at channel 1's pitch (+12), not channel 2's");
  e->destroy(inst);
}

void TestWavetables(const mpc_engine_t* e) {
  printf("wavetables (source = wavetables, no input):\n");
  for (int q = 0; q < 4; ++q) {
    void* inst = Fresh(e, q, 1, 0);
    Set(e, inst, "density", 0.3f);
    Set(e, inst, "feedback", q / 4.0f);   // bank
    Result r = Run(e, inst, SILENCE, 1.0f);
    char label[64];
    snprintf(label, sizeof label, "resolution %d", q);
    Print(label, r);
    Check(r.rms_signal > 0.01, "wavetable synth sounds without input");
    e->destroy(inst);
  }
}

void TestModulation(const mpc_engine_t* e) {
  printf("modulation (LFO 1 -> shape, -> seed):\n");
  void* inst = Fresh(e, 1, 0, 0);
  Set(e, inst, "dry_wet", 1);
  Set(e, inst, "density", 0.1f);   // clocked by the LFO: every pulse (12 o'clock would be probability 0)
  Set(e, inst, "lfo1_shape", 4);   // square
  Set(e, inst, "lfo1_rate", 0.6f);
  Set(e, inst, "mod1_src", 1);
  Set(e, inst, "mod1_dst", 10);    // Seed
  Set(e, inst, "mod1_amt", 100);
  Set(e, inst, "mod2_src", 1);
  Set(e, inst, "mod2_dst", 3);     // Shape
  Set(e, inst, "mod2_amt", 40);
  Result r = Run(e, inst, NOISE_BURSTS, 1.0f);
  Print("Cold Digital, LFO square on Seed", r);
  Check(r.rms_signal > 0.01, "LFO on Seed plays grains");
  e->destroy(inst);
}

void TestState(const mpc_engine_t* e) {
  printf("state chunk and displays:\n");
  void* a = e->create(NULL);
  Set(e, a, "quality", 3);
  Set(e, a, "pitch", 0.83f);
  Set(e, a, "mod3_amt", -42);
  char state[4096];
  int n = e->get_param(a, "state", state, sizeof state);
  void* b = e->create(NULL);
  e->set_param(b, "state", state);
  char va[32], vb[32];
  e->get_param(b, "quality", vb, sizeof vb);
  e->get_param(a, "quality", va, sizeof va);
  Check(n > 0 && !strcmp(va, vb), "state round trip (quality)");
  e->get_param(b, "mod3_amt", vb, sizeof vb);
  Check(!strcmp(vb, "-42"), "state round trip (mod3_amt)");
  const char* keys[] = { "pitch_display", "size_display", "density_display", "env1_attack_display",
                         "lfo1_rate_display", "feedback_name", "density_name" };
  for (const char* k : keys) {
    char buf[32] = "";
    int len = e->get_param(b, k, buf, sizeof buf);
    printf("  %-22s %s\n", k, len > 0 ? buf : "(none)");
    Check(len > 0, k);
  }
  printf("  state: %d bytes\n", n);
  e->destroy(a);
  e->destroy(b);
}

}  // namespace

int main() {
  const mpc_engine_t* e = mpc_engine();
  TestDryPath(e);
  TestQualities(e);
  TestDelay(e);
  TestFreeze(e);
  TestSeed(e);
  TestMidiSplit(e);
  TestWavetables(e);
  TestModulation(e);
  TestState(e);
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
