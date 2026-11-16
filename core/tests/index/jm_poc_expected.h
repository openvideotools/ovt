// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// JM-recorded picture order counts of the JVT conformance streams of the WI-1.111 acceptance tests: one token per
// decoded picture in decode order, "<structure><PicOrderCnt>" with structure F (frame), T (top field) or B (bottom
// field). Recorded on the Mac Mini runner (2026-11-15) with stock JM 19.0 ldecod (tests/oracles) and FFmpeg 7.1.1
// `-debug pict`: JM's per-picture table gives the POC of each frame and of the second field of each field pair and
// the decoded picture count ("N frames are decoded", one per field); FFmpeg's first-slice lines give the structure
// and the first field's POC (FFmpeg's 65536 bias of pic_order_cnt_type 0 removed, its probe decodes dropped). Every
// JM row matched the FFmpeg-derived tokens. The token count equals JM's decoded picture count.

#ifndef OVT_TESTS_INDEX_JM_POC_EXPECTED_H
#define OVT_TESTS_INDEX_JM_POC_EXPECTED_H

namespace ovt::test::h264 {

inline constexpr const char* kJm_CVPCMNL1_SVA_C =
    "F0 F2 F4 F6 F8 F10 F12 F14 F16 F18 F20 F22 F24 F26 F28 F30 F32 F34 F36 F38 F40 F42 F44 F46 F48 F50 "
    "F52 F54 F56 F58 ";

inline constexpr const char* kJm_MR6_BT_B =
    "T0 B1 T2 B3 T4 B5 T6 B7 T8 B9 T10 B11 T12 B13 T14 B15 T16 B17 T18 B19 T20 B21 T22 B23 T24 B25 T26 "
    "B27 T28 B29 T30 B31 T32 B33 T34 B35 T36 B37 T38 B39 T40 B41 T42 B43 T44 B45 T46 B47 T48 B49 T50 B51 "
    "T52 B53 T54 B55 T56 B57 T58 B59 T60 B61 T62 B63 T64 B65 T66 B67 T68 B69 T70 B71 T72 B73 T74 B75 T76 "
    "B77 T78 B79 T80 B81 T82 B83 T84 B85 T86 B87 T88 B89 T90 B91 T92 B93 T94 B95 T96 B97 T98 B99 T100 "
    "B101 T102 B103 T104 B105 T106 B107 T108 B109 T110 B111 T112 B113 T114 B115 T116 B117 T118 B119 ";

inline constexpr const char* kJm_MR9_BT_B =
    "F0 F6 F2 F4 F12 F8 F10 F18 F14 F16 F24 T20 B21 T22 B23 F30 F26 F28 F36 F32 F34 F42 T38 B39 F40 F48 "
    "F44 F46 F54 F50 F52 F60 F56 F58 F66 F62 F64 F72 F68 F70 F78 F74 F76 F84 T80 B81 T82 B83 F90 T86 B87 "
    "F88 F96 F92 F94 F102 F98 F100 F108 F104 F106 F114 F110 F112 ";

inline constexpr const char* kJm_CVFI1_Sony_D =
    "T0 B1 T2 B3 T4 B5 T6 B7 T8 B9 T10 B11 T12 B13 T14 B15 T16 B17 T18 B19 T20 B21 T22 B23 T24 B25 T26 "
    "B27 T28 B29 T30 B31 T32 B33 ";

inline constexpr const char* kJm_sp1_bt_a =
    "F0 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 "
    "F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 "
    "F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 "
    "F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 "
    "F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 "
    "F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 "
    "F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 "
    "F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 "
    "F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 "
    "F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 "
    "F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 "
    "F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 "
    "F4 F5 F6 F7 F8 F9 F11 F1 F2 F3 F4 F5 F6 F7 F8 F9 ";

}  // namespace ovt::test::h264

#endif  // OVT_TESTS_INDEX_JM_POC_EXPECTED_H
