# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `log` module of libovt-core (chapter 7 §7.1.6): ovt::log, the OVT_LOG macro and the ovt/status.h
# strings (WI-1.512). Sources go into ovt_core_objects, which ovt_core absorbs.

target_sources(ovt_core_objects PRIVATE log.cpp status_strings.cpp)
