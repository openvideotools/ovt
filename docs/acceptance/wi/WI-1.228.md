# WI-1.228 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3 (tests) and Clang 18 (fuzz), FFmpeg/ffprobe 6.1.1.
Date: 2026-11-08. Spec version: 1.0-rc15.

The full CMake configure could not download the pinned archives here, so the module, tests and fuzz target were
compiled by hand with `-fsanitize=address,undefined`. CI runs the real presets.

## Test 1: 30 frames of `src_1080p_10bit.y4m`

`src_1080p_10bit.y4m` comes from the chapter 14 §14.3.3 source recipe, which is not in the tree yet. The automated
test `y4m: 30 frames of 1080p 10-bit` (`[container][y4m]`) builds a sparse file of the same shape (header
`W1920 H1080 F30:1 Ip A1:1 C420p10`, 30 `FRAME` lines, plane bytes as holes) and checks `frame_count == 30`,
`bit_depth == 10`, `chroma_format == 1`, `frame_bytes == 1920 * 1080 * 2 * 1.5`.

The reader was also compared with `ffprobe -count_frames` on files written by FFmpeg for each `C` value it can
write (`y4m: frame_count and format equal ffprobe`, `OVT_Y4M_FFPROBE_FILE`): `frame_count`, width, height,
`pix_fmt` and frame rate equal ffprobe for `C411`, `C420jpeg`, `C422`, `C444`, `C444alpha`, `Cmono`, `C420p10`,
`C422p12`, `C444p16`, `Cmono10`, `Cmono12`:

```console
$ ffmpeg -f lavfi -i testsrc=size=66x50:rate=30000/1001 -frames:v 6 -pix_fmt yuv422p12le -strict -1 c422p12.y4m
$ OVT_Y4M_FFPROBE_FILE=c422p12.y4m ./ovt_tests_container "y4m: frame_count*"
All tests passed (6 assertions in 1 test case)
```

`C420p9`, which FFmpeg writes for `yuv420p9le`, is outside the §8.6.5 table and opens as `OVT_E_UNSUPPORTED`.

## Test 2: corrupted 5th `FRAME` magic

Automated: `y4m: corrupted FRAME magic` checks `CNT-Y4M-0003` and `frame_count == 4`.

## Fuzzing

```console
$ clang++-18 -fsanitize=fuzzer,address,undefined ... fuzz_y4m.cpp y4m.cpp ... -o fuzz_y4m
$ ./fuzz_y4m corpus -max_total_time=60 -max_len=16384
Done 166275 runs in 61 second(s)
```

No finding. CI runs the 60 s job (chapter 14 §14.7).
