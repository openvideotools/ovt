# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `decode` module of libovt-core (chapter 0 §0.3). Its work items add sources to ovt_core_objects here.
# WI-4.106: the decoded picture hashes of H.265 D.3.19 (picture_hash.cpp), shared with WI-4.543 and WI-4.109.

target_sources(ovt_core_objects PRIVATE picture_hash.cpp)
