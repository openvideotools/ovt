/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
   SPDX-License-Identifier: LGPL-2.1-or-later

   WI-0.087: ovt/decode.h and ovt/color.h compile as C11 (chapter 12 §12.3.1 rule 1). Built as an object
   library; there is nothing to run. */
#include <stddef.h>

#include "ovt/ovt.h"

_Static_assert(sizeof(ovt_ColorDescription) == 44, "backbone 0.5.6");
_Static_assert(offsetof(ovt_Picture, struct_size) == 0, "chapter 12 rule 7");
_Static_assert(offsetof(ovt_PlaneView, struct_size) == 0, "chapter 12 rule 7");

int ovt_c_api_decode_compile_check(const ovt_Picture* p);
int ovt_c_api_decode_compile_check(const ovt_Picture* p) {
  const ovt_FrameId id = p->id;
  return (int)p->planes[0].stride_bytes + id.decode_index + p->color.transfer;
}
