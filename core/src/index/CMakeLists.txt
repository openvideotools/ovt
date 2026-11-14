# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Placeholder for the `index` module of libovt-core (chapter 0 §0.3). Its work items add sources to
# ovt_core here with target_sources(ovt_core PRIVATE ...).

# WI-1.115: H.265 frame boundary and POC models (chapter 7 §7.3.9, §7.3.10.2).
target_sources(ovt_core_objects PRIVATE h265_frame_boundary.cpp h265_poc.cpp)
