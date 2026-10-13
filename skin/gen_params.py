#!/usr/bin/env python3
"""Writes ../params.json: the module's controls, then 2 envelopes, 2 LFOs, 8 matrix slots and the manual topic.

The order is the VST parameter order: it must match enum Param / kParams in src/engine.cc (and, for the modulation
part, InitModParams()). The option lists match the enums in src/engine.cc and src/mod.h. Once released, only append.

    python3 skin/gen_params.py
"""
import json
import os

QUALITIES = ["Bright Digital", "Cold Digital", "Sunny Tape", "Scorched Cassette"]
SEED_INPUTS = ["Off", "MIDI", "Clock 1 Bar", "Clock 1/2", "Clock 1/4", "Clock 1/8", "Clock 1/16", "Clock 1/32"]
ENV_TRIGGERS = ["MIDI", "LFO 1", "LFO 2"]   # (mod.h also has TRIG_BUTTON: the Seed button; not offered)
LFO_SHAPES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Rnd"]
DIVISIONS = ["4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32"]
SOURCES = ["Off", "LFO 1", "LFO 2", "Env 1", "Env 2", "Velocity"]
DESTS = ["Off", "Time", "Size", "Shape", "Pitch", "Density", "Feedback", "Dry/Wet", "Reverb", "Freeze", "Seed"]
MANUAL_PAGES = ["Basics", "Grains", "Seed", "Delay", "Qualities", "Wavetables", "MIDI", "Modulation"]


def knob(key, name, default, **extra):
    p = {"key": key, "name": name, "min": 0.0, "max": 1.0, "default": default}
    p.update(extra)
    return p


def amount(key, name):
    return {"key": key, "name": name, "min": -100.0, "max": 100.0, "unit": "%", "display": "int", "default": 0.0}


def base_params():
    return [
        {"key": "quality", "name": "Quality", "options": QUALITIES, "default": 0},
        knob("density", "Density", 0.3, dynamic_name=True, dynamic_display=True),
        knob("time", "Time", 0.0, dynamic_name=True),
        knob("size", "Size", 0.55, dynamic_display=True),
        knob("shape", "Shape", 0.5),
        knob("pitch", "Pitch", 0.5, dynamic_display=True),
        amount("time_amt", "Time Random"),
        amount("size_amt", "Size Random"),
        amount("shape_amt", "Shape Random"),
        amount("pitch_amt", "Pitch Random"),
        knob("feedback", "Feedback", 0.0, dynamic_name=True),
        knob("dry_wet", "Dry/Wet", 0.5, dynamic_name=True),
        knob("reverb", "Reverb", 0.0),
        {"key": "freeze", "name": "Freeze", "options": ["Off", "On"], "default": 0},
        {"key": "seed", "name": "Seed", "min": 0.0, "max": 1.0, "momentary": True, "type": "trigger", "default": 0.0,
         "hold_ms": 30},
        {"key": "seed_mode", "name": "Seed Mode", "options": ["Latched", "Gated"], "default": 0},
        {"key": "seed_input", "name": "Seed Input", "options": SEED_INPUTS, "default": 0},
        {"key": "source", "name": "Source", "options": ["Input", "Wavetables"], "default": 0},
        {"key": "buffer", "name": "Buffer", "options": ["Stereo", "Mono"], "default": 0},
        {"key": "midi_pitch", "name": "MIDI Pitch", "options": ["Off", "On"], "default": 1},
        {"key": "midi_split", "name": "MIDI Split", "options": ["Off", "On"], "default": 0},
        {"key": "midi_root", "name": "Root Note", "min": 0, "max": 127, "display": "int", "default": 60},
        {"key": "in_gain", "name": "In Gain", "min": -18.0, "max": 18.0, "unit": "dB", "default": 0.0,
         "dynamic_display": True},
        {"key": "out_gain", "name": "Out Gain", "min": -12.0, "max": 24.0, "unit": "dB", "default": 0.0,
         "dynamic_display": True},
    ]


def mod_params():
    out = []
    for e in (1, 2):
        out += [
            {"key": "env%d_attack" % e, "name": "Env %d Attack" % e, "min": 0.0, "max": 1.0, "default": 0.1,
             "dynamic_display": True},
            {"key": "env%d_decay" % e, "name": "Env %d Decay" % e, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "env%d_sustain" % e, "name": "Env %d Sustain" % e, "min": 0.0, "max": 1.0, "default": 0.7},
            {"key": "env%d_release" % e, "name": "Env %d Release" % e, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "env%d_trig" % e, "name": "Env %d Trigger" % e, "options": ENV_TRIGGERS, "default": 0},
        ]
    for l in (1, 2):
        out += [
            {"key": "lfo%d_shape" % l, "name": "LFO %d Shape" % l, "options": LFO_SHAPES, "default": 0},
            {"key": "lfo%d_rate" % l, "name": "LFO %d Rate" % l, "min": 0.0, "max": 1.0, "default": 0.5,
             "dynamic_display": True},
            {"key": "lfo%d_sync" % l, "name": "LFO %d Sync" % l, "options": ["Off", "On"], "default": 0},
            {"key": "lfo%d_div" % l, "name": "LFO %d Division" % l, "options": DIVISIONS, "default": 4},
        ]
    for m in range(1, 9):
        out += [
            {"key": "mod%d_src" % m, "name": "Mod %d Source" % m, "options": SOURCES, "default": 0},
            {"key": "mod%d_dst" % m, "name": "Mod %d Dest" % m, "options": DESTS, "default": 0},
            {"key": "mod%d_amt" % m, "name": "Mod %d Amount" % m, "min": -100.0, "max": 100.0, "unit": "%",
             "display": "int", "default": 0.0},
        ]
    # last: the MANUAL tab's topic (skin only)
    out.append({"key": "manual_page", "name": "Manual", "options": MANUAL_PAGES, "default": 0})
    return out


def main():
    params = base_params() + mod_params()
    keys = [p["key"] for p in params]
    sections = [
        {"label": "Grains", "keys": ["density", "time", "size", "shape", "pitch", "time_amt", "size_amt", "shape_amt",
                                     "pitch_amt", "freeze", "seed", "seed_mode", "seed_input"]},
        {"label": "Mix", "keys": ["feedback", "dry_wet", "reverb", "quality", "buffer", "source", "in_gain",
                                  "out_gain", "midi_pitch", "midi_split", "midi_root"]},
        {"label": "Envelopes", "keys": [k for k in keys if k.startswith("env")]},
        {"label": "LFOs", "keys": [k for k in keys if k.startswith("lfo")]},
        {"label": "Matrix", "keys": [k for k in keys if k.startswith("mod")]},
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "params.json")
    with open(path, "w") as f:
        json.dump({"name": "Drips", "params": params, "sections": sections}, f, indent=2)
        f.write("\n")
    print("params.json: %d params" % len(params))


if __name__ == "__main__":
    main()
