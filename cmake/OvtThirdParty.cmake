# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Dispatcher for third-party dependencies (chapter 13 §13.3). For each dependency it will either include
# third_party/<dep>.cmake (an ExternalProject build into OVT_THIRD_PARTY_PREFIX) or, when the matching
# OVT_SYSTEM_<DEP> option is ON, find the system library. The scripts arrive with WI-0.010 and later;
# until then this file only loads the shared helpers so that every build already goes through it.

include_guard(GLOBAL)

include(OvtThirdPartyCommon)   # ExternalProject conventions (§13.3.4), filled by WI-0.010
include(OvtMeson)              # Meson wrapper (§13.3.5), filled by WI-0.011

