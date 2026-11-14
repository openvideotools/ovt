# WI-4.205 acceptance record

WI-4.205 is split per §16.2.6: **a** (this record's first part) is section reassembly, CRC, PAT / PMT / CAT, the program
map and the PSI diagnostics (steps 1, 2 and 5); **b** is the descriptor registry, DVB SDT / EIT / TDT / TOT and the
section tree (steps 3 and 4). Acceptance test 2 belongs to part a; acceptance test 1 (the `tstables` oracle) to part b.

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0 with ASan/UBSan, Catch2 3.4.0.
Date: 2026-11-14. Spec question: #434.

## Part a, test 2: corrupted CRC

Automated: `psi: a corrupted CRC reports CNT-TS-2002 and the table is not applied`
(`[container][mpegts][psi][fast]`, `core/tests/container/psi_test.cpp`): the last byte of a PMT's CRC_32 is flipped;
`CNT-TS-2002` is emitted once, the section is kept with `crc_ok == false` and the program gets no streams.

## Part a, extra check against ffprobe (the oracle comparison of test 1 is part b)

`corpus: tsduck-test PMT lists the program streams that ffprobe reports` for corpus `ts_h264_dvb` (`input/test-012.ts`):

```console
$ ffprobe -v error -show_programs -show_streams -of compact test-012.ts
program|program_id=4006|program_num=4006|nb_streams=6|pmt_pid=160|pcr_pid=1060|...
stream|index=0|codec_name=h264|...|codec_tag=0x001b
stream|index=1..4|codec_name=mp3|...|codec_tag=0x0004
stream|index=5|codec_name=dvb_teletext|...|codec_tag=0x0006|...|id=0x42c
```

The parser reports program 4006, PMT PID 160, PCR PID 1060 and six streams: 0x1B on PID 1060, four 0x04, 0x06 on 0x42C.
The file carries packets for only three of the six PIDs, so `CNT-TS-1007` is reported for 0x424..0x427 and 0x42B; the
WI-4.204 corpus test now expects exactly those five diagnostics instead of none.

## Other checks

- Section reassembly: `psi: a section that spans packets and several sections in one packet`.
- Version replacement: `psi: a new version replaces the table, repeats are counted`; `current_next_indicator 0`.
- Diagnostics: `psi: PAT_error_2 and PMT_error_2`, `psi: PID_error ...`, `psi: CAT_error ...`.
- Fuzz: the existing `fuzz_mpegts` target walks every input through `parse`, so it covers the PSI parser; new seed
  `seed_5_psi.bin`. libFuzzer is not installed here; the same function was driven with 24 000 mutated inputs of the six
  seeds under ASan/UBSan without a finding. CI runs the 60 s fuzz job.

## Local run

```console
$ g++ -std=c++20 -fsanitize=address,undefined … psi_test.cpp … && OVT_CORPORA_DIR=… ./psi_test
All tests passed (283 assertions in 11 test cases)
$ ./ts_packets_test
All tests passed (242 assertions in 17 test cases)
$ ./c_api_test
All tests passed (66 assertions in 6 test cases)
```

(The container has no vcpkg toolchain, so the local run compiled the container module and these three test files
directly with the distribution's Catch2, flatbuffers and fmt; CI runs the real `ovt_tests_container`.)
