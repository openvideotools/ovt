# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Placeholder for the `syntax` module of libovt-core (chapter 0 §0.3). Its work items add sources to
# ovt_core here with target_sources(ovt_core PRIVATE ...).

# Shared user-data registries (chapter 7 §7.3.11, WI-1.120).
target_sources(ovt_core_objects PRIVATE common/user_data_registry.cpp common/t35_hdr10plus.cpp common/t35_atsc_cc.cpp)

# Unit scanners: Annex B, length-prefixed and AV1 OBU streams (chapter 7 §7.3.3, WI-1.107).
target_sources(ovt_core_objects PRIVATE common/unit_scan.cpp)
