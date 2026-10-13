#!/usr/bin/env python3
"""Writes ../layout.conf: three MPC tabs on the user's background (skin/drips_bg.jpg).

DRIPS: a 4x4 block laid out like the Q-Links (grain knobs, their attenurandomizers, density and the mix, gains and
freeze), on the right Quality and a big Seed pad. Rows are 117 px apart: just enough that the knobs' frames (knob,
name, value) don't touch, which would confuse MPC's arrow-key navigation.
SETTINGS: Seed mode and input, MIDI pitch and split, source, buffer, each with a short explanation.
MODULATION: two envelopes and two LFOs on the left, the 8-slot modulation matrix on the right (as in Overcast).
MANUAL: a topic list and its text.
skin/post_build.py removes MPC's own name labels, so the baked names below don't show twice.

The knob, tag and snowflake images are Overcast's for now (placeholders until Drips' own graphics arrive).

    python3 skin/gen_layout.py
"""
import json
import os

# The background has a title band at the top and a border band at the bottom; controls stay in between:
# layout y 196..662 (image y 110..576).
BACKGROUND = "art file=skin/drips_bg.jpg"

# DRIPS tab: the 4x4 block, row by row as on the Q-Links (None: the cell holds something else)
COLS = [110, 360, 610, 860]   # the block takes about 3/4 of the width
ROWS = [224, 341, 458, 575]
R = 26                # every knob in the block, the RANDOM knobs too
GRID = [["time", "size", "shape", "pitch"],
        ["time_amt", "size_amt", "shape_amt", "pitch_amt"],
        ["density", "feedback", "reverb", "dry_wet"],
        ["in_gain", "out_gain", None, None]]   # Freeze in the third cell
NAMES = {"time": "TIME", "size": "SIZE", "shape": "SHAPE", "pitch": "PITCH", "time_amt": "RANDOM",
         "size_amt": "RANDOM", "shape_amt": "RANDOM", "pitch_amt": "RANDOM", "density": "DENSITY",
         "feedback": "FEEDBACK", "dry_wet": "DRY/WET", "reverb": "REVERB", "in_gain": "IN GAIN",
         "out_gain": "OUT GAIN"}
# Coloured knobs and tags: the four grain parameters teal, density raspberry (placeholder colours)
COLOUR = {"time": "teal", "size": "teal", "shape": "teal", "pitch": "teal", "density": "raspberry"}
SEED_SIZE = 220
RIGHT_EDGE = 1245     # right edge of "texture synthesizer" in the background: Quality and the Seed pad end there
RIGHT_X = RIGHT_EDGE - SEED_SIZE // 2   # both 220 wide
TAG_W, TAG_H = 132, 26
NAME_GAP = 14         # knob edge (r) to the centre of its name
MOD_KNOB_R = 24       # all knobs on the MODULATION tab (envelopes and LFO rates)
SECTION_X, SECTION_W, SECTION_H = 55, 90, 34   # ENV / LFO tags: taller, for bigger lettering

# Q-Links: listed column by column, top to bottom (shadow_skin maps the list onto MPC's 4x4 grid that way):
# rows Time Size Shape Pitch / their RANDOM knobs / Density Feedback Reverb Quality / In Gain Out Gain Freeze Dry/Wet
# (Quality and Dry/Wet swapped against the screen, as the user asked: Dry/Wet in the bottom-right corner).
QLINKS_MAIN = ["time", "time_amt", "density", "in_gain",
               "size", "size_amt", "feedback", "out_gain",
               "shape", "shape_amt", "reverb", "freeze",
               "pitch", "pitch_amt", "quality", "dry_wet"]
QLINKS_SETTINGS = ["seed_mode", "seed_input", "midi_pitch", "midi_split", "source", "buffer"]
QLINKS_MOD = ["env1_attack", "env1_decay", "env1_sustain", "env1_release",
              "env2_attack", "env2_decay", "env2_sustain", "env2_release",
              "lfo1_rate", "lfo1_shape", "lfo2_rate", "lfo2_shape",
              "mod1_amt", "mod2_amt", "mod3_amt", "mod4_amt"]

THEME = [
    "art_css=skin/drips.css",
    # ink = popup list entries (and MPC's own name labels, removed by post_build.py); ink_dim = the value text
    "theme_bg=e7e7ec", "theme_panel=e7e7ec", "theme_line=b9b9c6", "theme_ink=1d1d22", "theme_ink_dim=4a4a55",
    "theme_ink_faint=b9b9c6", "theme_accent=c83d58", "theme_accent_hi=c83d58", "theme_knob_face=e9e9ee",
    "theme_knob_ring=d7d7de", "theme_knob_dot=1d1d22", "theme_lcd=ffffff", "theme_seg_active=009797",
    "theme_seg_inactive=f4f4f7", "theme_seg_active_tx=ffffff", "theme_box=ffffff", "theme_btn_bg=c83d58",
    "theme_btn_text=ffffff", "theme_btn_text_plain=ffffff",
]

INK = "1d1d22"

# MANUAL tab: a topic list on the left, the topic's text on the right (when=manual_page:<i>)
MANUAL = [
    ("BASICS", [
        "Drips records the track's audio into a buffer and replays it as grains: up to 30 at once, each with its own",
        "position, length, envelope and pitch, followed by a reverb. Built on Emilie Gillet's open-source DSP.",
        "",
        "TIME: where the grains start: left = the newest audio, right = the oldest (up to 32 s back).",
        "SIZE: grain length. Shortest (8 ms) at 10 o'clock, 4 s at both ends; left of 10 the grains play reversed.",
        "SIZE fully right turns Drips into a delay (see DELAY).",
        "SHAPE: grain envelope: left hard and clicky, middle a smooth bell, right a slow swell like reversed grains.",
        "PITCH: -24 ... +24 semitones, with notches at 0, 1, 3, 4, 7, 12 and 24. DENSITY: see GRAINS.",
        "FREEZE (snowflake): stops recording; the grains keep playing what is in the buffer.",
        "Grain settings are read when a grain starts: a change affects the next grains, not the ones playing.",
    ]),
    ("GRAINS", [
        "DENSITY at 12 o'clock: no grains (only the SEED button / input makes them).",
        "Left of 12: grains at a steady rate, faster further left (up to the period of a C3 note).",
        "Right of 12: grains at random times, more of them further right.",
        "",
        "RANDOM knobs (attenurandomizers): random variation of each grain's TIME, SIZE, SHAPE or PITCH,",
        "picked when the grain starts. Right of 12 o'clock: evenly spread random values. Left: a peaky",
        "source, mostly small changes with occasional big ones. In delay mode they are slow random LFOs.",
        "The more randomness (RANDOM knobs, random density), the wider the grains spread in stereo.",
        "",
        "Modulation from the matrix is added to the knob, like a CV with the RANDOM knob fully right.",
    ]),
    ("SEED", [
        "SEED MODE and SEED INPUT are on the SETTINGS tab; the big SEED pad is on the DRIPS tab.",
        "LATCHED (default): grains run continuously at the DENSITY rate.",
        "GATED: grains only while SEED is down (a tap = 30 ms) or the Seed input is high, e.g.",
        "while a MIDI note is held. At 12 o'clock each tap or note plays exactly one grain.",
        "",
        "SEED INPUT, the module's SEED jack: MIDI (notes from a MIDI track, see MIDI) or the MPC tempo",
        "(Clock 1 Bar ... 1/32). With a clock and LATCHED, DENSITY changes meaning:",
        "right of 12 o'clock = probability that a clock pulse plays a grain (0 ... 100 %),",
        "left of 12 o'clock = clock divider (fully left every pulse ... near 12 every 16th pulse).",
        "The matrix destination SEED works as a Seed input too.",
    ]),
    ("DELAY", [
        "Turn SIZE fully right: one endless grain reads the buffer continuously, Drips becomes a delay",
        "(it fades over in about 1.5 s).",
        "",
        "No Seed input, LATCHED: DENSITY sets the delay time. 12 o'clock = the whole buffer; turning",
        "either way shortens it, down to audio rate (flanging, combs). Right of 12 adds an uneven extra tap.",
        "Seed input (Clock = tempo-synced delay) or SEED taps in GATED: the time between pulses is the",
        "base time; DENSITY picks a subdivision (left: binary only, right: more ratios).",
        "FREEZE off: TIME multiplies the delay time. FREEZE on: TIME picks a slice that loops (beat slicer).",
        "SHAPE: a synced envelope on each repeat (fully left = plain delay). PITCH: rotary-head pitch shifter",
        "(off at 12 o'clock). The RANDOM knobs add slow random modulation.",
    ]),
    ("QUALITIES", [
        "The recording quality sets the sound and the buffer length (stereo / mono):",
        "",
        "BRIGHT DIGITAL  48 kHz 16-bit: clean.  4 s / 8 s",
        "COLD DIGITAL  32 kHz 12-bit: the classic, slightly lo-fi grain sound.  8 s / 16 s",
        "SUNNY TAPE  24 kHz 12-bit, dry signal at 48 kHz, tape saturation with IN GAIN.  10 s / 20 s",
        "SCORCHED CASSETTE  24 kHz 8-bit u-law, saturation and wow & flutter.  16 s / 32 s",
        "",
        "BUFFER MONO doubles the time (the input is summed to mono; the grains still pan in stereo).",
        "Changing quality, buffer or source clears the buffer. While frozen, the change waits until FREEZE is off.",
        "Each quality limits the feedback in its own way; Drips resamples to the module's own rate, as on hardware.",
    ]),
    ("WAVETABLES", [
        "SOURCE WAVETABLES: no input needed. Drips makes grains from built-in wavetables,",
        "like the module does when nothing is patched into it.",
        "",
        "FEEDBACK selects the wavetable bank. TIME: the position in the bank (which waveform).",
        "DRY/WET: the plain oscillator against the grains. SIZE, SHAPE and DENSITY work as with audio.",
        "PITCH (and MIDI notes with MIDI PITCH on) sets the root note; RANDOM randomises the grains' pitch.",
        "FREEZE halts the grain envelopes and stops new grains.",
        "QUALITY selects the output resolution: Bright Digital 16-bit, Cold Digital 7-bit,",
        "Sunny Tape 16-bit dry / 4-bit wet, Scorched Cassette 4-bit.",
    ]),
    ("MIDI", [
        "MPC OS sends no MIDI to insert effects, so every Drips opens its own MIDI input:",
        "Drips 1, Drips 2, ... (the number counts the instances in the order they were inserted).",
        "",
        "To play it: make a MIDI track and set its MIDI output to Drips 1. Notes are the module's two jacks:",
        "PITCH (V/OCT) with MIDI PITCH on, transposing relative to C3, and SEED with SEED INPUT = MIDI.",
        "MIDI SPLIT on (SETTINGS): channel 1 only transposes, channel 2 only seeds, like a sequencer's CV and",
        "gate patched into the module: one track plays the melody, another the rhythm.",
        "Envelopes set to MIDI and the velocity source follow the seeding notes.",
        "",
        "Classic: SEED INPUT MIDI, DENSITY at 12 o'clock, FREEZE on: every note plays a grain at its pitch.",
    ]),
    ("MODULATION", [
        "Two envelopes, two LFOs and an 8-slot matrix, like patch cables into the module's CV inputs.",
        "",
        "ENV 1 / ENV 2: attack, decay, sustain, release. TRIGGER: MIDI notes, or LFO 1 / LFO 2 (each cycle).",
        "LFO 1 / LFO 2: seven shapes. SYNC off: the knob sets the rate in Hz. SYNC on: it locks to the MPC",
        "tempo and picks a note value from 4 bars to 1/32.",
        "",
        "MATRIX: source -> destination -> amount (-100 to +100 %). Sources: LFOs, envelopes, note velocity.",
        "Several slots on one destination add up. FREEZE and SEED are gates: on above 50 %.",
        "",
        "Q-Links on this tab: ENV 1, ENV 2, the LFO rates and shapes, the amounts of slots 1 to 4.",
    ]),
]


def tag(cx, cy, colour, w=TAG_W, h=TAG_H):
    """Pill images keep their aspect ratio on screen, so other sizes need their own image (pill_<colour>_section)."""
    return "art file=skin/pill_%s.png x=%d y=%d w=%d h=%d" % (colour, cx - w // 2, cy - h // 2, w, h)


def text(cx, cy, label, colour, size=1.5, when=None, weight=700, align=None):
    line = 'text cx=%d cy=%d label="%s" size=%s weight=%d color=%s' % (cx, cy, label, size, weight, colour)
    line += " align=%s" % align if align else ""
    return line + (" when=" + when if when else "")


def name(cx, cy, label, colour, size=1.5):
    """A control's name: white on a coloured tag, or plain dark lettering."""
    out = [tag(cx, cy, colour)] if colour else []
    return out + [text(cx, cy, label, "ffffff" if colour else INK, size=size)]


def knob(key, cx, cy, r, label=None, colour="auto"):
    colour = COLOUR.get(key) if colour == "auto" else colour
    img = colour if colour in ("raspberry", "teal") else "white"
    return (name(cx, cy + r + NAME_GAP, label or NAMES[key], colour, size=1.5 if r >= R else 1.3) +
            ['knob cx=%d cy=%d r=%d label="" key=%s img=skin/knob_%s.png' % (cx, cy, r, key, img)])


def toggle(key, cx, cy, label):
    return name(cx, cy + 13 + 14, label, None) + ['toggle cx=%d cy=%d label="" key=%s' % (cx, cy, key)]


def heading(cx, cy, label):
    return text(cx, cy, label, INK, size=1.2)


def main_page():
    L = ["[tab DRIPS]", BACKGROUND]
    for r, row in enumerate(GRID):
        for c, key in enumerate(row):
            if key:
                L += knob(key, COLS[c], ROWS[r], R)
    L.append('toggle cx=%d cy=%d label="" key=freeze img=skin/freeze_off.png img_on=skin/freeze_on.png w=64 h=64'
             % (COLS[2], ROWS[3]))
    L.append(text(COLS[2], ROWS[3] + 32 + 12, "FREEZE", INK))
    # right: Quality, then the Seed pad (its label drawn on the pad)
    L += [heading(RIGHT_X, 222, "QUALITY"), 'popup cx=%d cy=256 w=220 h=40 label="" key=quality' % RIGHT_X]
    L.append('button cx=%d cy=%d label="SEED" key=seed img=skin/seed_off.png img_on=skin/seed_on.png w=%d h=%d'
             % (RIGHT_X, 450, SEED_SIZE, SEED_SIZE))
    L.append('qlinks "DRIPS" = ' + ",".join(QLINKS_MAIN))
    return L


# SETTINGS tab: one row per setting: name, control, a short explanation (one or two lines)
SETTINGS = [
    ("SEED MODE", 'enum_h cx=%d cy=%d label="" key=seed_mode sw=110 options="LATCHED,GATED"',
     ["Latched: grains run all the time.", "Gated: only while SEED is down or a seeding note is held."]),
    ("SEED INPUT", 'popup cx=%d cy=%d w=220 h=40 label="" key=seed_input',
     ["The module's SEED jack: MIDI notes or the MPC tempo.", "With a clock, DENSITY divides or sets a probability."]),
    ("MIDI PITCH", 'toggle cx=%d cy=%d label="" key=midi_pitch',
     ["Notes transpose the grains relative to C3 (V/OCT)."]),
    ("MIDI SPLIT", 'toggle cx=%d cy=%d label="" key=midi_split',
     ["Off: every note transposes and seeds.", "On: channel 1 transposes, channel 2 seeds."]),
    ("SOURCE", 'enum_h cx=%d cy=%d label="" key=source sw=130 options="INPUT,WAVETABLES"',
     ["Input: the track's audio. Wavetables: built-in", "waveforms, no input needed (FEEDBACK picks the bank)."]),
    ("BUFFER", 'enum_h cx=%d cy=%d label="" key=buffer sw=110 options="STEREO,MONO"',
     ["Mono doubles the recording time.", "Changing source, buffer or quality clears the buffer."]),
]


def settings_page():
    L = ["[tab SETTINGS]", BACKGROUND]
    for i, (label, control, hint) in enumerate(SETTINGS):
        cy = 238 + i * 76
        L.append(text(80, cy, label, INK, size=1.5, align="left"))
        L.append(control % (470, cy))
        for j, line in enumerate(hint):
            L.append(text(660, cy + (j - (len(hint) - 1) / 2.0) * 26, line, "4a4a55", size=1.3, weight=600,
                          align="left"))
    L.append('qlinks "SETTINGS" = ' + ",".join(QLINKS_SETTINGS))
    return L


def mod_page():
    L = ["[tab MODULATION]", BACKGROUND]
    # left: two envelopes and two LFOs, one row each, the section name in front of the row
    rows = [238, 359]       # 121 apart, so the rows' 116 px knob frames don't touch (touching frames confused MPC's
                            # arrow-key navigation)
    lfo_rows = [506, 611]
    ex = [170, 295, 420, 545]
    for e in (1, 2):
        cy = rows[e - 1]
        L += [tag(SECTION_X, cy, "raspberry_section", SECTION_W, SECTION_H), text(SECTION_X, cy, "ENV %d" % e, "ffffff", size=2)]
        for i, (p, lab) in enumerate((("attack", "ATTACK"), ("decay", "DECAY"), ("sustain", "SUSTAIN"),
                                      ("release", "RELEASE"))):
            L += knob("env%d_%s" % (e, p), ex[i], cy, MOD_KNOB_R, lab, colour=None)
        L += [heading(690, cy - 30, "TRIGGER"),
              'popup cx=690 cy=%d w=124 h=36 label="" key=env%d_trig' % (cy, e)]
    for l in (1, 2):
        # one compact row: shape list, sync toggle, rate knob whose value shows Hz or, synced, the note value
        cy = lfo_rows[l - 1]
        L += [tag(SECTION_X, cy, "teal_section", SECTION_W, SECTION_H), text(SECTION_X, cy, "LFO %d" % l, "ffffff", size=2)]
        L.append('popup cx=205 cy=%d w=150 h=36 label="" key=lfo%d_shape' % (cy, l))
        L += toggle("lfo%d_sync" % l, 345, cy - 8, "SYNC")
        L.append('knob cx=465 cy=%d r=%d label="" key=lfo%d_rate img=skin/knob_white.png' % (cy - 10, MOD_KNOB_R, l))
    # right: the matrix
    x0 = 770
    L += [text(x0 + 100, 208, "SOURCE", "636370", size=1.2), text(x0 + 260, 208, "DESTINATION", "636370", size=1.2),
          text(x0 + 420, 208, "AMOUNT", "636370", size=1.2)]
    for k in range(1, 9):
        cy = 238 + (k - 1) * 54
        L += [text(x0 + 8, cy, str(k), INK, size=1.4),
              'popup cx=%d cy=%d w=140 h=36 label="" key=mod%d_src' % (x0 + 100, cy, k),
              'popup cx=%d cy=%d w=150 h=36 label="" key=mod%d_dst' % (x0 + 260, cy, k),
              # the slider bar (y 40-60 of its 100 px frame) starts level with the destination box's top edge
              'slider_h cx=%d cy=%d w=100 h=20 label="" key=mod%d_amt' % (x0 + 420, cy - 8, k)]
    L.append('qlinks "MODULATION" = ' + ",".join(QLINKS_MOD))
    return L


def manual_page():
    L = ["[tab MANUAL]", BACKGROUND,
         'enum_v cx=150 cy=430 label="" key=manual_page sw=220 options="%s"' % ",".join(t for t, _ in MANUAL)]
    for i, (title, lines) in enumerate(MANUAL):
        when = "manual_page:%d" % i
        L.append(text(320, 214, title, "c83d58", size=1.8, when=when, align="left"))
        for j, line in enumerate(lines):
            if line:
                L.append(text(320, 254 + j * 35, line.replace('"', "'"), INK, size=1.45, when=when, weight=600,
                              align="left"))
    L.append('qlinks "MANUAL" = manual_page')
    return L


def qlink_bounds():
    """Per tab, the screen area of each Q-Link column (MPC X 4x4 grid; the MPC highlights it while a Q-Link turns).
    Skin coordinates are layout y - 86. Written to skin/qlink_bounds.json for post_build.py."""
    def rect(x0, y0, x1, y1):
        return "%d %d %d %d" % (x0, y0 - 86, x1 - x0, y1 - y0)
    main = [rect(c - 82, 196, c + 82, 662) for c in COLS]
    settings = [rect(0, 196, 1, 197)] * 4
    mod = [rect(110, 208, 610, 326), rect(110, 327, 610, 445), rect(10, 466, 530, 660), rect(760, 220, 1260, 420)]
    manual = [rect(30, 196, 270, 664)] + [rect(0, 196, 1, 197)] * 3
    return [main, mod, settings, manual]


def main():
    for _, lines in MANUAL:
        assert len(lines) <= 11, lines[0]
    L = ["# Drips skin. Generated by skin/gen_layout.py -- edit that, not this file."] + THEME + [""]
    L += main_page() + [""] + mod_page() + [""] + settings_page() + [""] + manual_page()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "layout.conf")
    open(path, "w").write("\n".join(L) + "\n")
    print("wrote", os.path.normpath(path))
    side = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qlink_bounds.json")
    json.dump(qlink_bounds(), open(side, "w"), indent=1)


if __name__ == "__main__":
    main()
