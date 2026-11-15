# WI-1.117 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, FFmpeg 6.1.1-3ubuntu5 (`trace_headers` bitstream
filter). Date: 2026-11-14. Spec version: 1.0-rc28. Commit: the head of the WI-1.117c pull request (#531).

Acceptance tests 1 and 3 and the hand-assembled half of acceptance test 2 are automated
(`core/tests/syntax/av1/test_syntax_av1_frame_header.cpp`: `av1_fh1_unit`, `av1_fh1_tiles_cdef_lr_first_header`,
`av1_frame_size_with_refs_found_ref`, `av1_fh1_inter_frame_short_signaling_and_ref_order_hint_mismatch`). This record
covers the corpus half of acceptance test 2, whose vector the automated test cannot read.

## Test 2 (corpus half): `av1-1-b8-03-sizeup` frame sizes across the sequence change

The fetched corpus entry `aom_av1_1_b8_03_sizeup` is `av1-1-b8-03-sizeup.mkv` (Matroska, sha256 `919bc6e93d1127a6…`).
The test `av1_fh1_sizeup_frame_sizes_follow_sequence_change` walks IVF files through the OBU scanner and
`parse_frame_header`; on this fixture it SKIPs by design ("av1-1-b8-03-sizeup is Matroska; the frame-header walk
needs the IVF form (demuxer: WI-1.231)"). Once the Matroska demuxer of WI-1.231 exists the test can feed the
SimpleBlock payloads instead and the record below becomes its expected values.

Oracle run (the OBUs are read from the Matroska blocks by FFmpeg; `trace_headers` prints every sequence and frame
header field):

```console
$ ffmpeg -hide_banner -loglevel trace -i av1-1-b8-03-sizeup.mkv -c:v copy -bsf:v trace_headers -f null - 2> sizeup.trace
$ grep -c '\] Frame Header$' sizeup.trace; grep -c '\] Sequence Header$' sizeup.trace
28
3
$ for k in show_existing_frame frame_type frame_size_override_flag max_frame_width_minus_1 max_frame_height_minus_1; do
    echo "== $k"; grep -E "\] +[0-9]+ +$k +[01]+ = " sizeup.trace | awk '{print $NF}' | sort -n | uniq -c; done
== show_existing_frame
     22 0
      6 1
== frame_type
      2 0
     20 1
== frame_size_override_flag
     22 0
== max_frame_width_minus_1
      2 351
      1 1279
== max_frame_height_minus_1
      2 287
      1 719
```

Stream order (sequence headers and the 28 frame headers; `SE` = `show_existing_frame = 1`, `K` = KEY_FRAME,
`I` = INTER_FRAME, the number is `order_hint`):

```text
SEQ 352x288 (codec private copy)  SEQ 352x288
K0 I9 I4 I2 I1 SE I3 SE I6 I5 SE I7 I8 I9
SEQ 1280x720
K0 I9 I4 I2 I1 SE I3 SE I6 I5 SE I7 I8 I9
```

Observations against the Then clause of acceptance test 2:

- `frame_size_override_flag` is 0 on all 22 non-show-existing frame headers, so `frame_size()` takes its sizes
  from the active sequence header on every frame and `frame_size_with_refs()` is never entered (#513; its
  coverage is the hand-assembled automated tests named above).
- The size change is a new sequence header (`max_frame_width_minus_1` 351 → 1279, `max_frame_height_minus_1`
  287 → 719) followed by a KEY frame, the second of the two KEY frames.
- Expected values of AV1 §5.9.5 / §5.9.8 / §5.9.9 for the two sequence headers, as `parse_frame_header` derives
  them (`enable_superres = 0`, so `UpscaledWidth = FrameWidth`):

  | sequence header | FrameWidth = UpscaledWidth | FrameHeight | MiCols = 2·((W+7)>>3) | MiRows = 2·((H+7)>>3) |
  |---|---|---|---|---|
  | 352×288 | 352 | 288 | 88 | 72 |
  | 1280×720 | 1280 | 720 | 320 | 180 |

  The automated test asserts exactly these relations (`FrameWidth == max_frame_width`,
  `MiCols == 2 * ((max_frame_width + 7) >> 3)`, widths `{352, 1280}`, 28 headers, 6 show-existing) and runs them
  on the IVF form of the vector.

Result: pass (manual record; the automated test is in place and skips only for the container format).
