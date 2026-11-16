<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# isobmff-broken

Small ISOBMFF files (about 20 KiB each, 260 KiB in all) with one deliberate defect in the box headers, used by
`core/tests/container/box_walk_test.cpp` (chapter 08 §8.2.3, §8.9.3, WI-1.201). Each is the base file of
`tools/make_broken.py` with one change; regenerate a file with the command in the table (the output is
byte-for-byte reproducible).

| File | `tools/make_broken.py` arguments | Expected code |
|---|---|---|
| `size7_in_moov.mp4` | `--box-size 7 --in moov` | `CNT-0106` |
| `size0_in_moov.mp4` | `--box-size 0 --in moov` | `CNT-0104` |
| `largesize_lt16_in_moov.mp4` | `--box-size 1 --in moov` | `CNT-0103` |
| `sizeffff_in_moov.mp4` | `--box-size 0xFFFFFFFF --in moov` | `CNT-0108` |
| `size0_top_level.mp4` | `--box-size 0 --in top` | none (legal at the top level) |
| `moov_exceeds_eof_by_1000.mp4` | `--moov-oversize 1000` | `CNT-0108` |
| `moov_after_mdat.mp4` | `--moov-after-mdat` | `CNT-0003` |
| `trak_overlaps_end_of_moov.mp4` | `--overlap` | `CNT-0108` |
| `nesting_33_deep.mp4` | `--nest 33` | `CNT-0105` |
| `uuid_size7.mp4` | `--uuid-size7` | `CNT-0106` |
| `uuid_short.mp4` | `--uuid-short` | `CNT-0107` |
| `nonprintable_type.mp4` | `--nonprintable` | `CNT-0109` |
| `trailing_5_bytes.mp4` | `--trailing 5` | `CNT-0101` |
| `truncated_at_1000.mp4` | `--truncate 1000` | `CNT-0108` |

The other truncation offsets and the 0/1/7/0xFFFFFFFF size words are exercised in memory by the test, which
mutates a base file of its own, so the committed set stays small.
