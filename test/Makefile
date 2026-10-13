# Native effect test (every quality, source and buffer; delay, freeze, Seed, wavetables, modulation, state chunk).
# No device, no Docker.
#   make -C test run
B = ../third_party/beads
SRC = fx_test.cc ../src/engine.cc ../src/midi_in.cc ../src/mod.cc \
      $(B)/beads/dsp/processor.cc $(B)/beads/dsp/synth.cc $(B)/beads/dsp/mu_law.cc $(B)/beads/dsp/delay_clock.cc \
      $(B)/beads/dsp/delay_controller.cc $(B)/beads/dsp/trigger_generator.cc $(B)/beads/resources.cc \
      $(B)/stmlib/dsp/units.cc $(B)/stmlib/dsp/atan.cc $(B)/stmlib/utils/random.cc
CXXFLAGS = -std=gnu++11 -O2 -g -DTEST -I$(B) -I../third_party/mpc-vst-plugins/wrapper \
           -fsanitize=address,undefined -Wall -Wno-unused-parameter -Wno-narrowing -Wno-unused-local-typedef \
           -Wno-vla-cxx-extension -Wno-unknown-warning-option

run: fx_test
	./fx_test

fx_test: $(SRC) ../src/resampler.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@

clean:
	rm -rf fx_test fx_test.dSYM

.PHONY: run clean
