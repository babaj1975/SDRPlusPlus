# Wideband sweep for PlutoSDR and bladeRF

This branch adds a local wideband spectrum and waterfall to the existing
`plutosdr_source` and `bladerf_source` modules. Device discovery, gain controls,
clock selection and the normal receiver remain in their upstream modules.

## Use

1. Build and load the PlutoSDR and/or bladeRF source module.
2. Select the receiver, sample rate and RF bandwidth, with reception stopped.
3. In **Wideband sweep**, set **Start (MHz)**, **Stop (MHz)**, **Step (MHz)**,
   **Dwell ms** and **Settle ms**, then check **Enable sweep**.
4. Start reception. The separate spectrum and waterfall appear in the source
   panel; the LO frequency, effective step and completed pass count are shown.
5. Stop reception before changing sweep settings, sample rate, bandwidth or
   clock. Uncheck **Enable sweep** and start again to resume normal reception
   at the frequency shown by the main SDR++ tuner.

The default range is 400–410 MHz, with a requested 1 MHz step, 20 ms dwell
and 10 ms settling. Choose frequencies supported by the actual receiver and
firmware. In particular, the older draft's 4900–6200 MHz example is **not** a
universal valid hardware range. Tune errors stop acquisition and appear in
the source panel/log; stop the source, correct the settings and restart.

The requested step is capped at 80% of the narrower of sample rate and RF
bandwidth. The first and last LO centers cover the requested display edges;
the final step can be shorter. A range narrower than the useful bandwidth
uses one LO center and still produces completed waterfall rows.

During a sweep, the main SDR++ spectrum, demodulators and IQ output receive
zeros. Hopping samples do not have a single center frequency and are displayed
only in the sweep view. Do not use an ordinary IQ recording made in this mode
as a recording of the swept RF spectrum.

Sweep settings are saved in the existing source configuration under `sweep`.
The enable checkbox is intentionally reset when SDR++ starts. The view and
sweep controls are local-GUI features; SDR++ server mode continues normal IQ
streaming and does not enable sweep.

## Acquisition and display

- Only the RX worker changes the LO during a sweep. Normal tuner changes are
  remembered for the next ordinary receive session.
- After each successful retune, the worker discards the configured driver
  queue (Pluto: two kernel buffers plus two conservative in-flight blocks;
  bladeRF: four sync buffers plus two transfers), then discards settling
  samples. Dwell counts accepted samples after this discard period. Timing is
  rounded up to whole RX blocks; total time per step also includes tuning and
  queue draining.
- The spectrum uses one Hann-windowed radix-2 FFT from the beginning of each
  accepted RX block, up to 2048 samples. Repeated observations and overlapping
  segments are combined by maximum power. This is an overview display, not a
  continuous full-rate signal detector or calibrated power meter.
- Each completed pass adds one waterfall row. The last completed spectrum
  remains visible while the next pass fills in. The display stores 1024 bins
  and 64 rows. The waterfall draws at most 128 columns, taking the peak of the
  corresponding bins. Hover over it to read frequency.
- Levels are relative dB using the modules' existing int16/32768 scaling and
  Hann coherent-gain correction. They are not dBm; receiver gain, sample
  format and AGC affect them. Manual gain is preferable when comparing levels
  across frequencies. Adjust **Floor (relative dB)** for visibility.
- Wideband coverage is sequential. Short signals can occur while the receiver
  is observing another segment or outside the FFT portion of a block. USB/IP
  throughput, PLL/AGC settling and analog response require hardware validation.

## Files and build

Shared header-only implementation:

- `source_modules/sweep/sweep_plan.h`: settings validation and LO coverage.
- `source_modules/sweep/sweep_spectrum.h`: FFT, stitching and synchronized history.
- `source_modules/sweep/sweep_controller.h`: acquisition state, UI and persistence.

The two source modules include these headers directly. No new external
library or change to the core signal path is required. The usual SDR++ build
dependencies still apply: libiio/libad9361 for Pluto and libbladeRF for bladeRF.
This uses the existing libiio 0.x API, as does the upstream Pluto module.

For an already configured Windows build with the existing dependencies:

```powershell
cmake -S . -B build -DOPT_BUILD_PLUTOSDR_SOURCE=ON -DOPT_BUILD_BLADERF_SOURCE=ON
cmake --build build --config Release --target plutosdr_source bladerf_source
```

Keep the toolchain and dependency paths from your existing build. All new
`std::min` and `std::max` calls are protected against Windows SDK macros;
there is no need to overwrite the updated device modules with the old drafts.

## Tests

The standalone tests require a C++17 compiler and CMake, with no SDR libraries,
connected receiver, OpenGL context or display server:

```sh
cmake -S tests/sweep -B build-sweep-tests
cmake --build build-sweep-tests --config Release
ctest --test-dir build-sweep-tests -C Release --output-on-failure
```

They check frequency coverage including fractional-bin edges, positive and
negative tones, Hann normalization, single-center and multi-center sweeps,
queue/settling discard, wraparound, invalid parameters, bounded history,
concurrent snapshots, hostile Windows-style min/max macros, and full-history
ImGui rendering with 16-bit draw indices. These tests do not establish RF
accuracy, driver queue latency, Windows/MSVC build success or physical device
compatibility. Check these on the target hardware before relying on measurements.
