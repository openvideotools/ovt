# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `bitreader` module of libovt-core (chapter 0 §0.3, chapter 7 §7.2): ovt::BitReader and ovt::ParseError
# (WI-1.102, AV1 descriptors WI-1.103), rbsp_from_nal and the raw <-> RBSP mapping (WI-1.104). Sources go into
# ovt_core_objects, which ovt_core absorbs (#62).

target_sources(ovt_core_objects PRIVATE bitreader.cpp bitreader_av1.cpp rbsp.cpp)
