// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Chapter 10 §10.7.6-§10.7.7 (WI-4.454): zeroes the scope counts before an accumulation (scopes.glsl). A
// pl_shader_custom header for a compute pass with 256 x 1 work groups (render/src/measure/scopes.cpp).
// Bindings: ovt_counts = uint storage buffer, ovt_n = its length, ovt_row = invocations per dispatch row.

void ovt_scope_clear() {
  uint i = gl_GlobalInvocationID.y * uint(ovt_row) + gl_GlobalInvocationID.x;
  if (i < uint(ovt_n)) {
    ovt_counts[i] = 0u;
  }
}
