// Read-only display parameters after the generated table (wrapper_adapter.h): the modulation dots around the DRIPS
// tab's knobs, the snowflake while the matrix freezes, and the RANDOM knobs the matrix switches off (*_lock). The engine answers get_param("view_<x>") (src/engine.cc);
// the skin binds them by index (NPARAMS + n, sdk/tools/mod_rings.py). Append only: the skin and the host know them by
// position.
#pragma once

#define WRAP_VIEWS 1

struct WrapView {
    const char* key;
    const char* name;
};

static const WrapView VIEWS[] = {
    {"view_time", "Time Mod"},
    {"view_size", "Size Mod"},
    {"view_shape", "Shape Mod"},
    {"view_pitch", "Pitch Mod"},
    {"view_density", "Density Mod"},
    {"view_feedback", "Feedback Mod"},
    {"view_dry_wet", "Dry/Wet Mod"},
    {"view_reverb", "Reverb Mod"},
    {"view_freeze", "Freeze Mod"},
    {"view_time_lock", "Time Random Off"},
    {"view_size_lock", "Size Random Off"},
    {"view_shape_lock", "Shape Random Off"},
    {"view_pitch_lock", "Pitch Random Off"},
};
#define NVIEWS ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))
