# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Placeholder created by WI-0.000 (chapter 0 §0.4). Each analysis plugin lives in plugins/ovt-plugin-<id>/ and is added here under its OVT_BUILD_PLUGIN_<ID> option (chapter 13 §13.9.4).

# ---- AV1 analysis plugin (libaom inspection, WI-2.155) --------------------------------------------------------
if(OVT_BUILD_PLUGIN_AOM)
  add_subdirectory(ovt-plugin-aom)
endif()

# WI-0.012 adds ovt-plugin-ffmpeg/ for the symbol-leak test of the FFmpeg plugin flavour; WI-2.169 adds the plugin.
if(OVT_BUILD_PLUGIN_FFMPEG)
  add_subdirectory(ovt-plugin-ffmpeg)
endif()

# ---- HEVC analysis plugin (libde265) --------------------------------------------------------------------
# WI-0.014 builds the patched libde265 (tp::de265); ovt-plugin-de265/ holds the plugin (WI-2.163) and its tests.
if(OVT_BUILD_PLUGIN_DE265 AND TARGET tp::de265)
  add_subdirectory(ovt-plugin-de265)
endif()

# ---- HM oracle (WI-4.104) -------------------------------------------------------------------------------------------
# WI-4.104 builds HM's decoder libraries (tp::hm) and their smoke test; the plugin itself (WI-4.105) adds
# ovt-plugin-hm/ here and moves the test under it.
if(OVT_BUILD_PLUGIN_HM AND OVT_BUILD_TESTS AND TARGET tp::hm)
  add_subdirectory(ovt-plugin-hm/tests)
endif()
