/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
   SPDX-License-Identifier: LGPL-2.1-or-later

   ovt/validate.h and ovt/syntax.h (#673, #674) compile as C11 (chapter 12 §12.3.1 rule 1) and are reachable
   through ovt/ovt.h. Built as an object library; there is nothing to run. */
#include <stddef.h>
#include <stdint.h>

#include "ovt/ovt.h"

ovt_status ovt_c_api_validate_compile_check(const char* json, ovt_Buffer* out);
ovt_status ovt_c_api_validate_compile_check(const char* json, ovt_Buffer* out) {
  ovt_Buffer fb = {NULL, 0};
  ovt_status st = ovt_json_to_fb(json, "FrameAnalysis", &fb);
  if (st != OVT_OK) {
    return st;
  }
  st = ovt_validate_frame_analysis(fb.data, fb.size, NULL, -1, out);
  if (st == OVT_OK) {
    ovt_buffer_free(out);
    st = ovt_validate_stream_index(fb.data, fb.size, out);
  }
  if (st == OVT_OK) {
    ovt_buffer_free(out);
    st = ovt_fb_to_json(fb.data, fb.size, "FrameAnalysis", 1, out);
  }
  ovt_buffer_free(&fb);
  return st;
}
