// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Stand-in for ovt_plugin_ffmpeg in the symbol-leak test (WI-0.012) until WI-2.169 builds the plugin. It links the
// static FFmpeg plugin flavour and exports the two plugin entry points of chapter 0 §0.6 as stubs. These are not
// the ABI declarations (core/include/ovt/abi/, WI-2.150): only the symbol names matter here.

#include <libavcodec/avcodec.h>

#define OVT_FIXTURE_EXPORT __attribute__((visibility("default")))

// Returns the h264 decoder of the private FFmpeg copy, so the archives are really linked in.
OVT_FIXTURE_EXPORT const void* ovt_analysis_decoder_v1(void) {
  return avcodec_find_decoder(AV_CODEC_ID_H264);
}

OVT_FIXTURE_EXPORT const char* ovt_plugin_describe(void) {
  return avcodec_configuration();
}
