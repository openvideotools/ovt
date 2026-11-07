<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->
# Known differences between the ISOBMFF oracles

Chapter 14 §14.4.6-§14.4.9, WI-0.564. The five oracles (MP4Box `-diso`, MP4Box `-dts`, `mp4ff-info`, MediaInfo
trace, mp4box.js) run on every `fast` ISOBMFF corpus file that can be fetched; `python -I
tools/goldens/gen_isobmff.py check` compares their `box-tree` goldens on `path` and `size` (all four box trees),
on `offset` and `header_length` (MediaInfo against mp4box.js, the only two that print them) and checks that every
`-dts` sample lies inside an `mdat`. Every disagreement is a row below; the check fails on a disagreement that
has no row and on a row that matches nothing. OVT is compared to these goldens only after this file is final.

Columns: corpus id (fnmatch), kind, box path (fnmatch, `?` stands for the `[` and `]` of a sibling index),
reason. Kinds: `no-golden:<oracle>` the oracle refused the file and there is no golden; `only-in:<oracle>` and
`missing-in:<oracle>` the box is listed by one oracle only or by all but one; `size`, `offset`,
`header_length` the numbers differ.

| id | kind | path | reason |
|---|---|---|---|
| `*` | `header_length` | `*/stsd/????` | mp4box.js counts the 8 fixed bytes after the box header of a sample entry (`reserved[6]`, `data_reference_index`) in `hdr_size`; MediaInfo and OVT's `BoxNode.header_length` do not. The normaliser cannot subtract them without knowing which boxes are entries (spec question #226). |
| `mpegff_10_fragments` | `no-golden:mp4ff-info` | `-` | mp4ff-info v0.48.0 refuses a file with more than one non-empty `mdat` ("only one non-empty mdat box supported"). |
| `mpegff_*` | `missing-in:mp4ff-info` | `*/stsd/mp4?/esds` | mp4ff does not parse the `esds` child of the MPEG-4 Visual and Systems sample entries `mp4v` and `mp4s`; it lists the unparsed entry without children. |
| `mpegff_hev1_clg1_header` | `only-in:mp4box-diso` | `*/hev1/colr` | The sample entry carries two `colr` boxes; GPAC keeps only one and prints it without a sibling index, the other three oracles list both (`colr[1]`, `colr[2]`). |
| `mpegff_hev1_clg1_header` | `missing-in:mp4box-diso` | `*/hev1/colr?1?` | Same file: the first `colr` as the other oracles list it. |
| `mpegff_hev1_clg1_header` | `missing-in:mp4box-diso` | `*/hev1/colr?2?` | Same file: the second `colr`. |
| `mpegff_hev1_clg1_segment` | `no-golden:mp4box-dts` | `-` | The file is a media segment without `moov`; MP4Box needs the initialisation segment (`mpegff_hev1_clg1_header`) to dump its timing. |
| `mpegff_hev1_clg1_segment` | `size` | `mdat` | MediaInfo's trace prints the `mdat` block with the 8-byte header only, all other oracles use the box size. |
| `mpegff_video_cenc_*` | `missing-in:mediainfo-trace` | `*/schi/tenc` | MediaInfo's trace does not print the `tenc` box inside `schi` as a block. |
| `gpac_*` | `missing-in:mediainfo-trace` | `*/schi/tenc` | Same as above. |
| `gpac_*` | `missing-in:mediainfo-trace` | `*/schi/uuid` | MediaInfo's trace does not print the `uuid` box inside `schi` as a block. |
| `ytugc_clip01` | `missing-in:mp4boxjs` | `moov/udta/meta/ilst/*too` | mp4box.js 1.5.0 does not parse the children of `ilst`: the `©too` item is not in `boxes` or `entries`. |
| `ytugc_clip01` | `missing-in:mp4boxjs` | `moov/udta/meta/ilst/*too/data` | Same: the `data` box of the item. |
| `gpac_*` | `header_length` | `*/uuid` | mp4box.js counts the 16-byte `usertype` of a `uuid` box in `hdr_size` (24); MediaInfo's Header block is 8 bytes. Whether OVT's `header_length` includes `usertype` is for the container chapter to say (spec question #226). |
