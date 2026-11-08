# Decoder safety regressions

After a Linux build with `OPT_BUILD_DSD=ON`, run:

```sh
bash scripts/test_dsd_modes.sh build
```

The test uses the same compiler/development libraries as the build. It needs
no receiver, sound device or display.

- `dsd_input_buffer.cpp` feeds the real oldDSD worker with 120- and 240-sample
  packets, then checks safe rejection of a 241-sample packet (ASan/UBSan).
  Before the fix, a 120-sample packet repeated twice produced:
  `dsd_demod.cpp:328: runtime error: index 240 out of bounds for type 'short int [240]'`.
- `dsd_status_concurrency.cpp` processes synthetic DMR sync/data while another
  thread copies NewDSD status, then exercises oldDSD's published status with a
  live input/output stream (ThreadSanitizer). Before the fix, TSan reported a
  concurrent string copy/write in `NewDSD::getDMRStatus()`.
- `dsd_mode_switch_stress.cpp` performs 440 transitions: every directed pair
  twice across NFM, WFM, AM, DSB, RAW, LSB, USB, CW, CW-R, DSD and oldDSD.
  The next demodulator is constructed while the previous one runs; audio is
  detached before stopping/deleting the previous object. ASan/UBSan check the
  actual demodulators and inline DSP lifecycle code with synthetic IQ input.

The harness instruments `dsd_demod.cpp` and the test's inline DSP code, and
links the remaining decoder routines from the freshly built `radio.so`.
The CI Noble build gates package upload on these regressions. CI disables
LeakSanitizer; bounds/lifetime checks and ThreadSanitizer remain enabled.

For the full application's GUI, VFO, IF/AF chains and configuration lifecycle,
also run (requires Xvfb, xauth and a Mesa software OpenGL driver):

```sh
bash scripts/test_radio_gui_modes.sh build
```

This compiles a temporary test-only source/controller module, feeds synthetic
IQ through the real frontend and switches the real Radio interface from the
GUI thread. Every directed pair runs twice while the menu/status is drawn.
The test checks selected mode, finite positive bandwidth and audio, normal shutdown and
preservation of distinct CW/CW-R tone settings. It reports switch latency and
resident memory after warm-up and at completion; these are diagnostics, not
a hardware performance guarantee. The module is never installed in packages.
Noble CI runs both harnesses while its build container is still active.

These tests reproduce two independent defects in the code. Without a crash
backtrace from the affected PC, they cannot establish which defect caused a
particular historical crash. Physical reception is not part of this harness.
