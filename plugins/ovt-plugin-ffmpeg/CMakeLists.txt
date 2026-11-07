# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ovt_plugin_ffmpeg, the AVC/HEVC codec pack (chapter 7 §7.8). WI-2.169 adds the plugin target here; until then the
# directory holds only the symbol-leak test of the FFmpeg plugin flavour (WI-0.012, chapter 13 §13.3.3).

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
