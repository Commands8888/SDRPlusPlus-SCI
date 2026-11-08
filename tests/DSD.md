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
- `dsd_mode_switch_stress.cpp` performs 270 transitions including
  DSD -> CW-R -> DSD and oldDSD -> CW-R -> oldDSD, plus NFM/LSB/USB.
  The next demodulator is constructed while the previous one runs; audio is
  detached before stopping/deleting the previous object. ASan/UBSan check the
  actual demodulators and inline DSP lifecycle code with synthetic IQ input.

The harness instruments `dsd_demod.cpp` and the test's inline DSP code, and
links the remaining decoder routines from the freshly built `radio.so`.
The CI Noble build gates package upload on these regressions. CI disables
LeakSanitizer; bounds/lifetime checks and ThreadSanitizer remain enabled.

These tests reproduce two independent defects in the code. Without a crash
backtrace from the affected PC, they cannot establish which defect caused a
particular historical crash. Physical reception is not part of this harness.
