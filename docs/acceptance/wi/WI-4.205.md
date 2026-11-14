# WI-4.205 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0 with ASan/UBSan, Catch2 3.4.0.
Date: 2026-11-14. Spec question: #434.

## Test 1: tsduck-test streams against `tstables --all-sections --xml`

TSDuck is not installed on this machine, so the oracle comparison is **not run here**. It is written as
`oracle: programs, PIDs and stream types equal tstables --all-sections --xml`
(`[container][mpegts][psi][needs-corpus][needs-oracle:tstables][fast]`, `core/tests/container/psi_test.cpp`): it runs
`tstables --all-sections --xml-output`, reads the `PAT` `service` and `PMT` `component` elements and compares program
numbers, PMT PIDs, PCR PIDs, stream types and elementary PIDs with the parser. It skips when `tstables` is absent. The
XML attribute names (`service_id`, `program_map_PID`, `PCR_PID`, `elementary_PID`, `stream_type`) come from the TSDuck
table schema and were not exercised locally; the first run in the oracle image is the check.

In place of it, `corpus: tsduck-test PMT lists the program streams that ffprobe reports` compares with `ffprobe` for
corpus `ts_h264_dvb` (`input/test-012.ts`):

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
**Owed:** the `tstables` run on a machine that has TSDuck, for this file and the other `input/*.ts` entries.

## Test 2: corrupted CRC

Automated: `psi: a corrupted CRC reports CNT-TS-2002 and the table is not applied`
(`[container][mpegts][psi][fast]`): the last byte of a PMT's CRC_32 is flipped; `CNT-TS-2002` is emitted once, the
section is kept with `crc_ok == false` and the program gets no streams.

## Other acceptance-relevant checks

- Section reassembly: `psi: a section that spans packets and several sections in one packet`.
- Version replacement: `psi: a new version replaces the table, repeats are counted`.
- Descriptors and DVB tables: `psi: ISO/IEC 13818-1 descriptors decode to named fields`,
  `psi: DVB SDT, EIT, TDT and TOT are decoded read-only`.
- Fuzz: `fuzz_mpegts` (existing target, which walks every file through `parse` and builds the tree) now also covers
  the PSI parser; new seed `seed_5_psi.bin`. libFuzzer is not installed here; the same function was driven with
  24 000 mutated inputs of the six seeds under ASan/UBSan without a finding. CI runs the 60 s fuzz job.

## Local run

```console
$ g++ -std=c++20 -fsanitize=address,undefined … psi_test.cpp … && OVT_CORPORA_DIR=… ./psi_test
All tests passed (psi_test + ts_packets_test together: 585 assertions in 29 test cases)   # oracle test skipped: tstables absent
$ ./ts_packets_test
All tests passed (242 assertions in 17 test cases)
$ ./c_api_test
All tests passed (66 assertions in 6 test cases)
```

(The container has no vcpkg toolchain, so the local run compiled the container module and these three test files
directly with the distribution's Catch2, flatbuffers and fmt; CI runs the real `ovt_tests_container`.)
