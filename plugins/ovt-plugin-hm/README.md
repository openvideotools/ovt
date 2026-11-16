<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: BSD-3-Clause
-->

# ovt-plugin-hm

The optional HEVC oracle analysis plugin (chapter 07 §7.6.4) over HM-18.0, the JCT-VC reference decoder
(`-DOVT_BUILD_PLUGIN_HM=ON`). Never the default H.265 plugin unless `de265` is absent or refuses a stream.

Licence and patents: the plugin is BSD-3-Clause. HM is BSD-3-Clause (ITU/ISO/IEC); its `COPYING`, installed to
`share/doc/hm/`, grants no patent rights. HEVC is covered by patent pools, so the plugin stays out of the default
downloads. HM's `libmd5` is public domain.

Limitations: HM handles malformed input with `exit()` and `assert()` and prints a line per picture to stdout; the
plugin checks only the NAL unit header, so analyse untrusted streams with `de265` (issue #442).

Analysis data: the CellGrid (all fields but `partition_id`, `bits_x8` and `deblock_bs`), the CTU/CU/PU/TU block tree
with HevcCtu/HevcCu/HevcPu/HevcTu extensions, motion vectors and reference slots. MotionVector.predictor is
`merge[i]` or `amvp[i]` without the `:<source>` suffix, because HM does not keep which candidate was chosen
(issue #495).
