# Vendored DSP

- `beads/dsp/`, `beads/resources.{cc,h}`: Mutable Instruments Beads by Emilie Gillet, from
  [pichenettes/eurorack](https://github.com/pichenettes/eurorack) commit `d8f473915232af74347762b6365202bfd26f73f6`
  (2026-10-12, "Beads code/sch"). MIT license (see the header of every file; the repository's README: "Code (STM32F
  projects): MIT license").
- `stmlib/`: [pichenettes/stmlib](https://github.com/pichenettes/stmlib) commit `e3bd7c9cc00e4364166f9905c0509b6ffd0535ec`
  (the revision eurorack pins), only `stmlib.h`, `dsp/` and `utils/`. MIT, see `stmlib/LICENSE`.

One change: `beads/dsp/wavetable_granular_player.h` printed a line for every stolen grain when built with `-DTEST`
(which the port needs: without it stmlib uses Cortex-M instructions). That trace now also needs
`BEADS_TRACE_GRAIN_STEALING`, so it stays out of the MPC's audio thread. Everything else is unmodified. The hardware side of the firmware (`beads.cc`, `ui.cc`, drivers, gain control, settings) is not used:
`src/engine.cc` replaces it.
