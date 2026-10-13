// Drips: Mutable Instruments Beads for MPC OS, as an insert effect.
//
// Replaces the firmware's main loop and UI (beads/beads.cc, ui.cc): pots, CV inputs, buttons and the SEED/FREEZE
// jacks become plugin params, MIDI notes and an 8-slot modulation matrix. The DSP (beads/dsp) is unmodified.
//
// Sample rate: Beads switches its codec between 24, 32 and 48 kHz with the recording quality, and the qualities'
// sound depends on it (Cold Digital's 32 kHz, the cassette's 24 kHz). The MPC runs at 44.1 kHz, so each block is
// resampled to the module's rate, processed in blocks of rate / 4000 frames (the module's 4 kHz block rate, at which
// it also reads its pots and CVs) and resampled back. Latency: about 1 ms.
//
// Changing the quality, buffer or source re-initialises the processor (clearing 768 KB of buffer) and the converters
// (their kernels): several ms, so it runs on a worker thread while the output is muted anyway.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <new>
#include <pthread.h>

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/gate_flags.h"
#include "midi_in.h"
#include "mod.h"
#include "resampler.h"

extern "C" {
#include "engine.h"
}

using namespace beads;
using drips::Resampler;
using drips::StereoFrame;

namespace {

const int kHostRate = 44100;
const int kChunk = 128;                 // host frames handled per pass (MPC blocks are 128 frames)
const int kMaxModuleBlock = 12;         // 48 kHz / 4 kHz
const int kModuleFifoSize = 512;
const int kOutFifoSize = 1024;
const int kDelayBankSize = 256 * 1024;  // the module's delay memory: 512 KB (RAM D1) + 256 KB (RAM D2), three banks
const int kReverbBufferSize = 32768;    // uint16_t words (RAM D3)
const float kPotSmoothing = 0.05f;      // per module block, the module's pot filter (cv_reader)
const int kReconfigureBlocks = 100;     // host blocks (~0.3 s) between two quality/buffer/source changes
const int kMuteBlocks = 8;              // host blocks of silence after a change (the module mutes 0.25 s)

enum Param {
  P_QUALITY, P_DENSITY, P_TIME, P_SIZE, P_SHAPE, P_PITCH,
  P_TIME_AMT, P_SIZE_AMT, P_SHAPE_AMT, P_PITCH_AMT,
  P_FEEDBACK, P_DRY_WET, P_REVERB,
  P_FREEZE, P_SEED, P_SEED_MODE, P_SEED_INPUT, P_SOURCE, P_BUFFER,
  P_MIDI_PITCH, P_MIDI_SPLIT, P_MIDI_ROOT, P_IN_GAIN, P_OUT_GAIN,
  // modulation page: per envelope attack, decay, sustain, release, trigger; per LFO shape, rate, sync,
  // division; per matrix slot source, destination, amount
  P_MOD_FIRST,
  P_ENV_FIRST = P_MOD_FIRST,
  P_LFO_FIRST = P_ENV_FIRST + mod::kNumEnvs * 5,
  P_SLOT_FIRST = P_LFO_FIRST + mod::kNumLfos * 4,
  P_MANUAL = P_SLOT_FIRST + mod::kNumSlots * 3,   // the MANUAL tab's topic; skin only, the engine ignores it
  P_LAST
};

enum Kind { CONTINUOUS, OPTION, MOMENTARY };

struct ParamInfo {
  const char* key;
  float def;
  Kind kind;
};

// Same list as params.json (names, ranges, labels), in VST order. The modulation entries are filled in by
// InitModParams(); params.json gets them from skin/gen_params.py.
ParamInfo kParams[P_LAST] = {
  { "quality", 0, OPTION },          // Bright Digital (the module's power-on setting)
  { "density", 0.3f, CONTINUOUS },   // counter-clockwise of 12 o'clock: a steady ~5 Hz grain rate
  { "time", 0.0f, CONTINUOUS },     // the most recent audio (the buffer is up to 32 s long)
  { "size", 0.55f, CONTINUOUS },     // forward grains of ~100 ms
  { "shape", 0.5f, CONTINUOUS },
  { "pitch", 0.5f, CONTINUOUS },     // knob position; 12 o'clock = 0 semitones
  { "time_amt", 0, CONTINUOUS },     // attenurandomizers, -100..+100 %
  { "size_amt", 0, CONTINUOUS },
  { "shape_amt", 0, CONTINUOUS },
  { "pitch_amt", 0, CONTINUOUS },
  { "feedback", 0, CONTINUOUS },
  { "dry_wet", 0.5f, CONTINUOUS },
  { "reverb", 0, CONTINUOUS },
  { "freeze", 0, OPTION },
  { "seed", 0, MOMENTARY },
  { "seed_mode", 0, OPTION },        // Latched (the module's power-on mode), Gated
  { "seed_input", 0, OPTION },       // Off, MIDI, Clock 1 Bar .. 1/32
  { "source", 0, OPTION },           // Input, Wavetables
  { "buffer", 0, OPTION },           // Stereo, Mono
  { "midi_pitch", 1, OPTION },      // notes transpose (V/OCT)
  { "midi_split", 0, OPTION },      // off: every note transposes and seeds; on: channel 1 transposes, channel 2 seeds
  { "midi_root", 60, OPTION },       // the note that plays at the Pitch knob's setting (integer)
  { "in_gain", 0.0f, CONTINUOUS },   // dB, into the module's input stage (tape saturation in the tape qualities)
  { "out_gain", 0.0f, CONTINUOUS },  // dB, into the limiter
};

enum SeedMode { SEED_LATCHED, SEED_GATED };

enum SeedInput { SEED_INPUT_OFF, SEED_INPUT_MIDI, SEED_INPUT_CLOCK_FIRST };
// Clock divisions of the Seed input, in quarter notes: 1 bar, 1/2, 1/4, 1/8, 1/16, 1/32.
const double kClockBeats[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
const int kNumClocks = sizeof(kClockBeats) / sizeof(kClockBeats[0]);

char mod_keys[P_MANUAL - P_MOD_FIRST][16];

void InitModParams() {
  static bool done = false;
  if (done) return;
  done = true;
  int k = 0;
  const char* env_keys[5] = { "attack", "decay", "sustain", "release", "trig" };
  const float env_defaults[5] = { 0.1f, 0.5f, 0.7f, 0.5f, mod::TRIG_MIDI };
  for (int e = 0; e < mod::kNumEnvs; ++e) {
    for (int j = 0; j < 5; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "env%d_%s", e + 1, env_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], env_defaults[j], j == 4 ? OPTION : CONTINUOUS };
    }
  }
  const char* lfo_keys[4] = { "shape", "rate", "sync", "div" };
  const float lfo_defaults[4] = { mod::SHAPE_SINE, 0.5f, 0, 4 };   // 4 = 1/4 note
  for (int l = 0; l < mod::kNumLfos; ++l) {
    for (int j = 0; j < 4; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "lfo%d_%s", l + 1, lfo_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], lfo_defaults[j], j == 1 ? CONTINUOUS : OPTION };
    }
  }
  const char* slot_keys[3] = { "src", "dst", "amt" };
  for (int m = 0; m < mod::kNumSlots; ++m) {
    for (int j = 0; j < 3; ++j, ++k) {
      snprintf(mod_keys[k], sizeof mod_keys[k], "mod%d_%s", m + 1, slot_keys[j]);
      kParams[P_MOD_FIRST + k] = { mod_keys[k], 0.0f, j == 2 ? CONTINUOUS : OPTION };   // amount in %
    }
  }
  kParams[P_MANUAL] = { "manual_page", 0, OPTION };
}

template<typename T> T Clamp(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

// The PITCH pot's law (cv_reader.h, LAW_DETUNE): -24..+24 semitones with notches at 0, 1, 3, 4, 7, 12 and 24.
float PitchKnobToSemitones(float knob) {
  float detune = 2.0f * Clamp(knob, 0.0f, 1.0f) - 1.0f;
  float sign = detune < 0.0f ? -1.0f : 1.0f;
  detune = detune * sign * 6.0f;
  int integral = static_cast<int>(detune);
  float fractional = detune - static_cast<float>(integral);
  const float intervals[8] = { 0.0f, 1.0f, 3.0f, 4.0f, 7.0f, 12.0f, 24.0f, 24.0f };
  if (integral > 6) integral = 6;
  fractional = fractional - 0.33333333f > 0.0f ? (fractional - 0.33333333f) * 1.5f : 0.0f;
  float a = intervals[integral];
  float b = intervals[integral + 1];
  return stmlib::Crossfade(a, b, stmlib::SmoothStep(fractional)) * sign;
}

// The attenurandomizer pots' law (LAW_ATTENUVERTER): a small dead zone around 12 o'clock. percent: -100..+100.
float AttenuverterLaw(float percent) {
  float v = Clamp(percent, -100.0f, 100.0f) / 200.0f;   // -0.5..0.5, as knob - 0.5
  float a = fabsf(v) - 0.03f;
  return (2.0f / 0.47f) * v * (a > 0.0f ? a : 0.0f);
}

struct Instance {
  Processor processor;
  Synth synth;
  uint8_t* delay_memory;      // three banks of kDelayBankSize
  uint16_t* reverb_buffer;

  float param[P_LAST];
  float pot[P_LAST];          // continuous params, smoothed per module block
  bool pots_started;

  // configuration in use; a change re-initialises the processor and the converters
  int media, synth_mode;
  bool mono;
  bool switching;             // a change waits for the output to fade out
  int rate, block;            // module sample rate, frames per module block (rate / 4000)
  int blocks_since_configure;
  int mute_blocks;
  float level;                // output fade (0 while switching)

  // the configuration worker: the audio thread sets configuring and stays silent (touching neither the processor nor
  // the converters) until the worker has run Configure() and cleared it
  std::atomic<bool> configuring;
  bool worker_running;
  bool worker_quit;
  pthread_t worker;
  pthread_mutex_t worker_mutex;
  pthread_cond_t worker_wake;

  Resampler down, up;
  StereoFrame module_in[kModuleFifoSize];
  int module_in_count;
  StereoFrame out_fifo[kOutFifoSize];
  int out_count;
  bool primed;                // enough output buffered to stream

  // Seed (the module's SEED button and jack)
  int seed_frames;            // module frames the button stays pressed after a tap
  bool seed_pressed_pending;  // a press not yet seen by a module block
  stmlib::GateFlags previous_seed;
  int note_min_frames;        // a note-on keeps the MIDI gate high at least this long
  bool note_retrigger;        // a note-on while the gate was high: one low frame for a new rising edge
  double beats;               // clock position in quarter notes (Seed Input = Clock)

  // MIDI
  midi_in::Port* midi;
  bool midi_seen;
  int notes_held;
  float note_offset;          // semitones from the last note-on, held after note-off (like a V/OCT CV)
  float velocity;
  bool midi_retrigger;        // for the envelopes

  mod::Modulation modulation;
  bool playing;               // MPC transport (HAS_HOST_TRANSPORT)
  double ppq, bpm;

  float limiter_peak;
};

int Option(const Instance* s, int p) { return static_cast<int>(s->param[p] + 0.5f); }

// The firmware's objects are globals, zeroed before Init(); instances live in zeroed memory too.
Instance* NewInstance() {
  void* mem = calloc(1, sizeof(Instance));
  return mem ? new (mem) Instance : NULL;
}

void DeleteInstance(Instance* s) {
  s->~Instance();
  free(s);
}

Processor::Media MediaOf(int quality) {
  // Quality list in the order the module's button steps through it: Bright Digital (48 kHz 16-bit),
  // Cold Digital (32 kHz 12-bit), Sunny Tape (24 kHz 12-bit), Scorched Cassette (24 kHz 8-bit).
  return Processor::Media(3 - Clamp(quality, 0, 3));
}

bool ModTargets(const Instance* s, int dest) {
  for (int k = 0; k < mod::kNumSlots; ++k) {
    const float* v = &s->param[P_SLOT_FIRST + k * 3];
    if (static_cast<int>(v[0] + 0.5f) != mod::SRC_OFF && static_cast<int>(v[1] + 0.5f) == dest && v[2] != 0.0f) {
      return true;
    }
  }
  return false;
}

// As the module does when the quality, the mono/stereo input or synth mode changes (MonitorStateChange()): the
// processor is configured afresh, which clears the recording buffer.
void Configure(Instance* s) {
  s->media = MediaOf(Option(s, P_QUALITY));
  s->mono = Option(s, P_BUFFER) == 1;
  s->synth_mode = Option(s, P_SOURCE) == 1;
  if (s->synth_mode) {
    s->rate = 48000;
  } else {
    s->processor.Configure(Processor::Media(s->media), s->mono);
    s->rate = static_cast<int>(s->processor.sample_rate());
  }
  s->block = s->rate / 4000;
  s->down.Init(kHostRate, s->rate);
  s->up.Init(s->rate, kHostRate);
  s->module_in_count = 0;
  s->out_count = 0;
  s->primed = false;
  s->blocks_since_configure = 0;
  fprintf(stderr, "Drips: %s, quality %d, %s, %d Hz\n", s->synth_mode ? "wavetables" : "input", Option(s, P_QUALITY),
          s->mono ? "mono" : "stereo", s->rate);
}

bool ConfigurationChanged(const Instance* s) {
  return MediaOf(Option(s, P_QUALITY)) != s->media || (Option(s, P_BUFFER) == 1) != s->mono ||
         (Option(s, P_SOURCE) == 1) != s->synth_mode;
}

void* ConfigureLoop(void* arg) {
  Instance* s = static_cast<Instance*>(arg);
  pthread_mutex_lock(&s->worker_mutex);
  while (!s->worker_quit) {
    if (s->configuring.load(std::memory_order_acquire)) {
      pthread_mutex_unlock(&s->worker_mutex);
      Configure(s);
      s->configuring.store(false, std::memory_order_release);
      pthread_mutex_lock(&s->worker_mutex);
      continue;
    }
    pthread_cond_wait(&s->worker_wake, &s->worker_mutex);
  }
  pthread_mutex_unlock(&s->worker_mutex);
  return NULL;
}

void* Create(const char* data_dir) {
  (void)data_dir;
  Instance* s = NewInstance();
  if (!s) return NULL;
  s->delay_memory = static_cast<uint8_t*>(calloc(3, kDelayBankSize));
  s->reverb_buffer = static_cast<uint16_t*>(calloc(kReverbBufferSize, sizeof(uint16_t)));
  if (!s->delay_memory || !s->reverb_buffer) {
    free(s->delay_memory);
    free(s->reverb_buffer);
    DeleteInstance(s);
    return NULL;
  }
  uint8_t* banks[3] = { s->delay_memory, s->delay_memory + kDelayBankSize, s->delay_memory + 2 * kDelayBankSize };
  s->processor.Init(banks, s->reverb_buffer);
  s->synth.Init(s->reverb_buffer);
  for (int p = 0; p < P_LAST; ++p) s->param[p] = s->pot[p] = kParams[p].def;
  s->previous_seed = stmlib::GATE_FLAG_LOW;
  s->midi = midi_in::Open();
  s->modulation.Init(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)));
  s->ppq = -1.0;
  s->bpm = 120.0;
  Configure(s);
  s->configuring.store(false);
  pthread_mutex_init(&s->worker_mutex, NULL);
  pthread_cond_init(&s->worker_wake, NULL);
  s->worker_running = pthread_create(&s->worker, NULL, ConfigureLoop, s) == 0;
  return s;
}

void Destroy(void* inst) {
  Instance* s = static_cast<Instance*>(inst);
  if (s->worker_running) {
    pthread_mutex_lock(&s->worker_mutex);
    s->worker_quit = true;
    pthread_cond_signal(&s->worker_wake);
    pthread_mutex_unlock(&s->worker_mutex);
    pthread_join(s->worker, NULL);
  }
  pthread_cond_destroy(&s->worker_wake);
  pthread_mutex_destroy(&s->worker_mutex);
  midi_in::Close(s->midi);
  free(s->delay_memory);
  free(s->reverb_buffer);
  DeleteInstance(s);
}

void Midi(void* inst, const uint8_t* msg, int len) {
  Instance* s = static_cast<Instance*>(inst);
  if (len < 3) return;
  if (!s->midi_seen) {
    s->midi_seen = true;
    fprintf(stderr, "Drips: MIDI received (%02x %02x %02x)\n", msg[0], msg[1], msg[2]);
  }
  uint8_t status = msg[0] & 0xf0;
  int channel = msg[0] & 0x0f;
  // Notes play the module's two jacks, PITCH CV (V/OCT) and SEED. Split, like patching a sequencer's CV and gate:
  // channel 1 only transposes, channel 2 only gates (Seed, envelopes, velocity).
  bool split = Option(s, P_MIDI_SPLIT) != 0;
  bool pitch = Option(s, P_MIDI_PITCH) != 0 && (!split || channel == 0);
  bool gate = !split || channel == 1;
  if (status == 0x90 && msg[2] > 0) {
    if (pitch) s->note_offset = static_cast<float>(msg[1] - Option(s, P_MIDI_ROOT));
    if (gate) {
      if (s->notes_held > 0 || s->note_min_frames > 0) s->note_retrigger = true;
      ++s->notes_held;
      s->note_min_frames = s->rate / 500;   // 2 ms
      s->velocity = msg[2] / 127.0f;
      s->midi_retrigger = true;
    }
  } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
    if (gate && s->notes_held > 0) --s->notes_held;
  }
}

void SetParam(void* inst, const char* key, const char* val);

// Project save/load: the wrapper stores get_param("state") as the plugin chunk ("key=value;...").
int SaveState(const Instance* s, char* buf, int buf_len) {
  int len = 0;
  for (int p = 0; p < P_LAST && len < buf_len; ++p) {
    if (kParams[p].kind == MOMENTARY) continue;
    len += snprintf(buf + len, buf_len - len, "%s=%g;", kParams[p].key, s->param[p]);
  }
  return len < buf_len ? len : buf_len - 1;
}

void LoadState(Instance* s, const char* state) {
  char item[64];
  while (*state) {
    size_t n = strcspn(state, ";");
    if (n < sizeof item) {
      memcpy(item, state, n);
      item[n] = 0;
      char* eq = strchr(item, '=');
      if (eq) {
        *eq = 0;
        if (strcmp(item, "state")) SetParam(s, item, eq + 1);
      }
    }
    state += n;
    if (*state == ';') ++state;
  }
}

void SetParam(void* inst, const char* key, const char* val) {
  Instance* s = static_cast<Instance*>(inst);
  if (!strcmp(key, "state")) {
    LoadState(s, val);
    return;
  }
  if (!strcmp(key, "host_transport")) {
    int playing = 0;
    double ppq = -1.0, bpm = 0.0;
    if (sscanf(val, "%d %lf %lf", &playing, &ppq, &bpm) >= 2) {
      s->playing = playing != 0;
      s->ppq = ppq;
      if (bpm > 0.0) s->bpm = bpm;
    }
    return;
  }
  for (int p = 0; p < P_LAST; ++p) {
    if (strcmp(key, kParams[p].key)) continue;
    float v = static_cast<float>(atof(val));
    // Every press is a tap of 30 ms: the wrapper reports a momentary param back to 0 to the host, but the engine
    // isn't guaranteed to hear the release.
    if (p == P_SEED && v > 0.5f) s->seed_pressed_pending = true;
    s->param[p] = v;
    return;
  }
}

const char* const kDivisionNames[] = { "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32" };

int LfoDivision(float rate_knob) { return Clamp(static_cast<int>(rate_knob * 7.0f + 0.5f), 0, 7); }

bool DelayMode(const Instance* s) { return s->pot[P_SIZE] > 0.99f && !s->synth_mode; }

// Readable values for MPC's parameter display (dynamic_display in params.json).
int Display(const Instance* s, int p, char* buf, int buf_len) {
  float v = s->param[p];
  switch (p) {
    case P_PITCH: {
      float st = PitchKnobToSemitones(v);
      return fabsf(st - roundf(st)) < 0.05f ? snprintf(buf, buf_len, "%+d st", static_cast<int>(roundf(st)))
                                            : snprintf(buf, buf_len, "%+.1f st", st);
    }
    case P_SIZE: {
      if (v > 0.99f) return snprintf(buf, buf_len, "Delay");
      // DelayGranularPlayer: below 1/3 reversed; 4 s at both ends, ~8 ms at 1/3
      bool reverse = v < 0.3333333f;
      float shortness = reverse ? v * 3.0f * (2.0f - v * 3.0f) : 1.0f - 1.5f * (v - 0.3333333f);
      shortness = fminf(1.0f, 1.025f * shortness);
      float ms = 4000.0f * powf(2.0f, -9.0f * shortness);
      if (ms >= 1000.0f) return snprintf(buf, buf_len, "%s%.1f s", reverse ? "Rev " : "", ms / 1000.0f);
      return snprintf(buf, buf_len, "%s%d ms", reverse ? "Rev " : "", static_cast<int>(ms + 0.5f));
    }
    case P_DENSITY: {
      if (DelayMode(s)) return snprintf(buf, buf_len, "%d %%", static_cast<int>(v * 100.0f + 0.5f));
      // TriggerGenerator: a steady rate below 0.48, random above 0.52, none in between (Seed only)
      if (v < 0.48f) return snprintf(buf, buf_len, "%.1f Hz", 130.81f * powf(2.0f, -v * 16.0f));
      if (v > 0.52f) return snprintf(buf, buf_len, "~%.1f Hz", 0.5f * 130.81f * powf(2.0f, -(1.0f - v) * 16.0f));
      return snprintf(buf, buf_len, "Seed only");
    }
    case P_IN_GAIN:
    case P_OUT_GAIN:
      return snprintf(buf, buf_len, "%+.1f dB", v);
    default:
      break;
  }
  if (p >= P_MOD_FIRST && p < P_SLOT_FIRST) {   // envelope times, LFO rates
    float k = Clamp(v, 0.0f, 1.0f);
    if (strstr(kParams[p].key, "_rate")) {
      int l = (p - P_LFO_FIRST) / 4;
      if (s->param[P_LFO_FIRST + l * 4 + 2] > 0.5f) return snprintf(buf, buf_len, "%s", kDivisionNames[LfoDivision(k)]);
      return snprintf(buf, buf_len, "%.2f Hz", 0.01f * powf(3000.0f, k));
    }
    float sec = 0.001f * powf(10000.0f, k);
    return sec < 1.0f ? snprintf(buf, buf_len, "%d ms", static_cast<int>(sec * 1000.0f + 0.5f))
                      : snprintf(buf, buf_len, "%.1f s", sec);
  }
  return 0;
}

// Names for MPC's Q-Link display (dynamic_name): the knobs' second functions, as in the manual.
const char* Name(const Instance* s, int p) {
  bool clocked = Option(s, P_SEED_INPUT) != SEED_INPUT_OFF || Option(s, P_SEED_MODE) == SEED_GATED;
  switch (p) {
    case P_FEEDBACK: return s->synth_mode ? "Bank" : "Feedback";
    case P_DRY_WET: return s->synth_mode ? "Osc/Grains" : "Dry/Wet";
    case P_DENSITY: return DelayMode(s) ? (clocked ? "Division" : "Delay Time") : "Density";
    case P_TIME: return DelayMode(s) ? (Option(s, P_FREEZE) ? "Slice" : "Repeats") : "Time";
    default: return NULL;
  }
}

int GetParam(void* inst, const char* key, char* buf, int buf_len) {
  const Instance* s = static_cast<const Instance*>(inst);
  if (!strcmp(key, "state")) return SaveState(s, buf, buf_len);
  if (!strcmp(key, "busy")) return snprintf(buf, buf_len, "%d", s->configuring.load() ? 1 : 0);   // for tests
  size_t key_len = strlen(key);
  for (int suffix = 0; suffix < 2; ++suffix) {
    const char* tail = suffix ? "_display" : "_name";
    size_t tail_len = strlen(tail);
    if (key_len <= tail_len || strcmp(key + key_len - tail_len, tail)) continue;
    for (int p = 0; p < P_LAST; ++p) {
      if (strlen(kParams[p].key) != key_len - tail_len || strncmp(key, kParams[p].key, key_len - tail_len)) continue;
      if (suffix) return Display(s, p, buf, buf_len);
      const char* name = Name(s, p);
      return name ? snprintf(buf, buf_len, "%s", name) : 0;
    }
    return 0;
  }
  for (int p = 0; p < P_LAST; ++p) {
    if (strcmp(key, kParams[p].key)) continue;
    if (kParams[p].kind != CONTINUOUS) return snprintf(buf, buf_len, "%d", Option(s, p));
    return snprintf(buf, buf_len, "%g", s->param[p]);
  }
  return 0;
}

// One control-rate step of the envelopes, LFOs and matrix.
const mod::Modulation& UpdateModulation(Instance* s, float dt) {
  mod::Settings ms;
  for (int e = 0; e < mod::kNumEnvs; ++e) {
    const float* v = &s->param[P_ENV_FIRST + e * 5];
    ms.env[e] = { v[0], v[1], v[2], v[3], static_cast<int>(v[4] + 0.5f) };
  }
  for (int l = 0; l < mod::kNumLfos; ++l) {
    const float* v = &s->param[P_LFO_FIRST + l * 4];
    // Synced, the rate knob picks the note value (0 = 4 bars .. 1 = 1/32); lfo<n>_div is not on the skin.
    ms.lfo[l] = { static_cast<int>(v[0] + 0.5f), v[1], v[2] > 0.5f, LfoDivision(v[1]) };
  }
  for (int k = 0; k < mod::kNumSlots; ++k) {
    const float* v = &s->param[P_SLOT_FIRST + k * 3];
    ms.source[k] = static_cast<int>(v[0] + 0.5f);
    ms.dest[k] = static_cast<int>(v[1] + 0.5f);
    ms.amount[k] = v[2] / 100.0f;   // -100..+100 %
  }
  mod::Inputs in = { s->notes_held > 0, s->midi_retrigger, s->seed_frames > 0, s->velocity, s->playing, s->ppq,
                     s->bpm };
  s->midi_retrigger = false;
  s->modulation.Process(ms, in, dt);
  return s->modulation;
}

// The module's input stage with manual gain (gain_control.cc): hard clipping in the digital qualities, a biased soft
// clipper in Scorched Cassette, and in Sunny Tape a blend of both that grows with the gain.
void InputStage(const Instance* s, const StereoFrame* in, FloatFrame* out, size_t n) {
  float gain = powf(10.0f, s->pot[P_IN_GAIN] / 20.0f);
  float soft = 0.0f;
  if (s->media == Processor::MEDIA_DIRTY_TAPE) soft = 1.0f;
  if (s->media == Processor::MEDIA_CLEAN_TAPE) soft = Clamp(2.0f * (gain - 0.75f), 0.0f, 1.0f);
  const float bias = 0.05f;
  for (size_t i = 0; i < n; ++i) {
    float l = in[i].l * gain;
    float r = in[i].r * gain;
    float soft_l = (1.0f - bias) * stmlib::SoftClip(l + bias) - bias;
    float soft_r = (1.0f - bias) * stmlib::SoftClip(r + bias) - bias;
    l = Clamp(l, -1.0f, 1.0f);
    r = Clamp(r, -1.0f, 1.0f);
    out[i].l = l + (soft_l - l) * soft;
    out[i].r = r + (soft_r - r) * soft;
  }
}

// One module block (rate / 4000 frames): what the module's audio interrupt does (Process() in beads.cc).
void RunModuleBlock(Instance* s, const StereoFrame* in, StereoFrame* out, size_t n) {
  // pots
  for (int p = 0; p < P_LAST; ++p) {
    if (kParams[p].kind != CONTINUOUS) continue;
    if (!s->pots_started) s->pot[p] = s->param[p];
    s->pot[p] += kPotSmoothing * (s->param[p] - s->pot[p]);
  }
  s->pots_started = true;

  const mod::Modulation& m = UpdateModulation(s, static_cast<float>(n) / static_cast<float>(s->rate));

  // grain parameters: knob + modulation (the matrix plays the CV inputs, added to the knob as with the attenurandomizer
  // fully clockwise); the attenurandomizers keep their unpatched function, internal randomisation
  Parameters p;
  const float* pot = s->pot;
  p.time.base = Clamp(pot[P_TIME] + m.out(mod::DST_TIME), 0.0f, 1.0f);
  p.size.base = Clamp(pot[P_SIZE] + m.out(mod::DST_SIZE), 0.0f, 1.0f);
  p.shape.base = Clamp(pot[P_SHAPE] + m.out(mod::DST_SHAPE), 0.0f, 1.0f);
  float note = Option(s, P_MIDI_PITCH) ? s->note_offset : 0.0f;
  p.pitch.base = PitchKnobToSemitones(pot[P_PITCH]) + note + 24.0f * m.out(mod::DST_PITCH);
  RandomizedParameter* rp[4] = { &p.time, &p.size, &p.shape, &p.pitch };
  for (int i = 0; i < 4; ++i) {
    rp[i]->attenuverter = AttenuverterLaw(pot[P_TIME_AMT + i]);
    rp[i]->cv = 0.0f;
    rp[i]->has_cv = false;
  }
  p.monophonic_mode = false;

  ModulatedParameter d;
  d.base = pot[P_DENSITY];
  d.cv = m.out(mod::DST_DENSITY);

  Processor::MixParameters mix;
  mix.feedback = Clamp(pot[P_FEEDBACK] * 1.005f + m.out(mod::DST_FEEDBACK), 0.0f, 1.0f);
  mix.dry_wet = Clamp(pot[P_DRY_WET] * 1.005f + m.out(mod::DST_DRY_WET), 0.0f, 1.0f);
  mix.reverb_amount = Clamp(pot[P_REVERB] * 1.005f + m.out(mod::DST_REVERB), 0.0f, 1.0f);
  mix.output_gate = false;

  bool freeze_on = Option(s, P_FREEZE) != 0 || m.out(mod::DST_FREEZE) > 0.5f;
  bool freeze[kMaxModuleBlock];
  for (size_t i = 0; i < n; ++i) freeze[i] = freeze_on;

  // SEED button: latched, or (Gated) pressed for 30 ms per tap: one grain at 12 o'clock, a tap tempo in delay mode
  SeedButtonState seed_button = SEED_BUTTON_STATE_LOCKED;
  if (s->seed_pressed_pending) {
    s->seed_pressed_pending = false;
    // a tap while the last one is still down: release it for this block so the new one is a new press
    s->seed_frames = s->seed_frames > 0 ? -1 : s->rate * 3 / 100;
  }
  if (Option(s, P_SEED_MODE) == SEED_GATED) {
    seed_button = s->seed_frames > 0 ? SEED_BUTTON_STATE_ON : SEED_BUTTON_STATE_OFF;
  }
  if (s->seed_frames < 0) {
    s->seed_frames = s->rate * 3 / 100;
  } else {
    s->seed_frames = s->seed_frames > static_cast<int>(n) ? s->seed_frames - static_cast<int>(n) : 0;
  }

  // SEED jack: MIDI notes, the MPC clock and/or the matrix's Seed destination. Patched only when one is in use, as
  // the module tells a patched jack from an empty one.
  int seed_input = Option(s, P_SEED_INPUT);
  bool mod_seed = ModTargets(s, mod::DST_SEED);
  stmlib::GateFlags seed[kMaxModuleBlock];
  const stmlib::GateFlags* seed_ptr = NULL;
  double beats_per_frame = s->bpm / 60.0 / s->rate;
  if (seed_input != SEED_INPUT_OFF || mod_seed) {
    bool mod_gate = m.out(mod::DST_SEED) > 0.5f;
    for (size_t i = 0; i < n; ++i) {
      bool gate = mod_gate;
      if (seed_input == SEED_INPUT_MIDI) {
        bool midi_gate = s->notes_held > 0 || s->note_min_frames > 0;
        if (s->note_retrigger) {
          midi_gate = false;   // one low frame, so a new note starts a new gate
          s->note_retrigger = false;
        }
        gate |= midi_gate;
      } else if (seed_input >= SEED_INPUT_CLOCK_FIRST) {
        double div = kClockBeats[Clamp(seed_input - SEED_INPUT_CLOCK_FIRST, 0, kNumClocks - 1)];
        double phase = s->beats / div;
        gate |= phase - floor(phase) < 0.5;
      }
      if (s->note_min_frames > 0) --s->note_min_frames;
      s->beats += beats_per_frame;
      seed[i] = s->previous_seed = stmlib::ExtractGateFlags(s->previous_seed, gate);
    }
    seed_ptr = seed;
  } else {
    s->beats += beats_per_frame * n;
    s->note_min_frames = 0;
    s->note_retrigger = false;
    s->previous_seed = stmlib::GATE_FLAG_LOW;
  }

  FloatFrame fin[kMaxModuleBlock], fout[kMaxModuleBlock];
  if (s->synth_mode) {
    s->synth.Render(seed_button, seed_ptr, freeze, Synth::Resolution(MediaOf(Option(s, P_QUALITY))), d, p, mix,
                    fout, n);
  } else {
    InputStage(s, in, fin, n);
    s->processor.Process(seed_button, seed_ptr, freeze, d, p, mix, fin, fout, n);
  }
  for (size_t i = 0; i < n; ++i) {
    out[i].l = Clamp(fout[i].l, -1.0f, 0.999999f);
    out[i].r = Clamp(fout[i].r, -1.0f, 0.999999f);
  }
}

void Render(void* inst, int16_t* out_lr, int frames) {
  (void)inst;
  memset(out_lr, 0, sizeof(int16_t) * 2 * frames);  // an effect: the wrapper calls process()
}

inline int16_t ToShort(float x) {
  x *= 32768.0f;
  return static_cast<int16_t>(x > 32767.0f ? 32767.0f : (x < -32768.0f ? -32768.0f : x));
}

// A soft knee above 0.9: the last safety net after the limiter.
inline float SoftSaturate(float x) {
  const float t = 0.9f;
  float a = fabsf(x);
  if (a <= t) return x;
  float over = (a - t) / (1.0f - t);
  float y = t + (1.0f - t) * over / (1.0f + over);   // approaches 1.0
  return x < 0.0f ? -y : y;
}

// One host chunk (at most kChunk frames): resample in, run module blocks, resample out, then output gain, a
// stereo-linked peak limiter at -1 dBFS (~150 ms release) and the fade used while switching configurations.
void ProcessChunk(Instance* s, const int16_t* in_lr, int16_t* out_lr, int frames) {
  StereoFrame host_in[kChunk];
  for (int i = 0; i < frames; ++i) {
    host_in[i].l = in_lr[2 * i] / 32768.0f;
    host_in[i].r = in_lr[2 * i + 1] / 32768.0f;
  }
  s->module_in_count += static_cast<int>(s->down.Process(host_in, frames, s->module_in + s->module_in_count));

  int offset = 0;
  while (s->module_in_count - offset >= s->block) {
    StereoFrame module_out[kMaxModuleBlock];
    RunModuleBlock(s, s->module_in + offset, module_out, s->block);
    offset += s->block;
    if (s->out_count + 16 < kOutFifoSize) {
      s->out_count += static_cast<int>(s->up.Process(module_out, s->block, s->out_fifo + s->out_count));
    }
  }
  s->module_in_count -= offset;
  memmove(s->module_in, s->module_in + offset, sizeof(StereoFrame) * s->module_in_count);

  // Start streaming once the output can't run dry: the module blocks complete output in steps of a block.
  int margin = (s->block * kHostRate + s->rate - 1) / s->rate + 4;
  if (!s->primed && s->out_count >= frames + margin) s->primed = true;
  int available = s->primed ? (s->out_count < frames ? s->out_count : frames) : 0;
  if (s->primed && available < frames) {
    fprintf(stderr, "Drips: output underrun (%d of %d frames)\n", available, frames);
    s->primed = false;
  }

  const float threshold = 0.891f;   // -1 dBFS
  const float release = 1.0f - 1.0f / (0.15f * kHostRate);
  const float gain = powf(10.0f, s->pot[P_OUT_GAIN] / 20.0f);
  const float target = s->mute_blocks > 0 || s->switching ? 0.0f : 1.0f;
  for (int i = 0; i < frames; ++i) {
    StereoFrame y = { 0.0f, 0.0f };
    if (i < available) y = s->out_fifo[i];
    s->level += Clamp(target - s->level, -1.0f / 256.0f, 1.0f / 256.0f);
    float l = y.l * gain * s->level;
    float r = y.r * gain * s->level;
    float peak = fmaxf(fabsf(l), fabsf(r));
    s->limiter_peak = peak > s->limiter_peak ? peak : s->limiter_peak * release;
    float g = s->limiter_peak > threshold ? threshold / s->limiter_peak : 1.0f;
    out_lr[2 * i] = ToShort(SoftSaturate(l * g));
    out_lr[2 * i + 1] = ToShort(SoftSaturate(r * g));
  }
  s->out_count -= available;
  memmove(s->out_fifo, s->out_fifo + available, sizeof(StereoFrame) * s->out_count);
}

void Process(void* inst, const int16_t* in_lr, int16_t* out_lr, int frames) {
  Instance* s = static_cast<Instance*>(inst);
  midi_in::Poll(s->midi, Midi, s);
  if (s->configuring.load(std::memory_order_acquire)) {   // the worker owns the processor and the converters
    memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
    return;
  }

  // Quality, buffer and source changes: fade out, re-initialise (clears the buffer), stay silent briefly, fade in.
  // As on the module, nothing changes while frozen (that would lose the frozen buffer); a turning Q-Link applies at
  // most every ~0.3 s.
  if (s->blocks_since_configure < kReconfigureBlocks) ++s->blocks_since_configure;
  if (s->mute_blocks > 0) --s->mute_blocks;
  s->switching = ConfigurationChanged(s) && Option(s, P_FREEZE) == 0;
  if (s->switching && s->level <= 0.0f && s->blocks_since_configure >= kReconfigureBlocks) {
    s->switching = false;
    s->mute_blocks = kMuteBlocks;
    if (s->worker_running) {
      s->configuring.store(true, std::memory_order_release);
      pthread_mutex_lock(&s->worker_mutex);
      pthread_cond_signal(&s->worker_wake);
      pthread_mutex_unlock(&s->worker_mutex);
      memset(out_lr, 0, sizeof(int16_t) * 2 * frames);
      return;
    }
    Configure(s);   // no worker thread: in the audio thread, as a fallback
  }
  // the Seed clock follows the song position while the MPC plays, and runs on at its tempo when stopped
  if (s->playing && s->ppq >= 0.0) s->beats = s->ppq;

  for (int offset = 0; offset < frames; offset += kChunk) {
    int n = frames - offset < kChunk ? frames - offset : kChunk;
    ProcessChunk(s, in_lr + 2 * offset, out_lr + 2 * offset, n);
  }
}

const mpc_engine_t kEngine = { Create, Destroy, Midi, SetParam, GetParam, Render, Process };

}  // namespace

extern "C" const mpc_engine_t* mpc_engine(void) {
  InitModParams();
  return &kEngine;
}
