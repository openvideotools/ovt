# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Configure-time check that a libaom was built with CONFIG_INSPECTION (chapter 13 §13.2.2 OVT_SYSTEM_LIBAOM,
# WI-0.013 step 4). Used for OVT_SYSTEM_LIBAOM=ON, where the distro libaom is usually built without it.
#
#   ovt_aom_require_inspection(<target>)
#       Compiles a probe against <aom/aomdx.h>, links it with <target> and runs it. The probe opens an AV1
#       decoder and registers a callback with AV1_SET_INSPECTION_CALLBACK; a libaom built without
#       CONFIG_INSPECTION answers AOM_CODEC_INCAPABLE (chapter 07 §7.6.1). Fails configure with
#       "libaom was not built with CONFIG_INSPECTION" when the probe does not report success.
#
# SPEC-ASSUMPTION(#152): the spec says the check "compiles a probe against aom/aomdx.h
# AV1_SET_INSPECTION_CALLBACK". The control id is declared in aomdx.h whether or not the library was built with
# CONFIG_INSPECTION, so compiling alone cannot tell; the probe is therefore also linked and run. A cross build
# cannot run it and is refused (OVT_SYSTEM_LIBAOM is meant for native distro builds).

include_guard(GLOBAL)

function(ovt_aom_require_inspection target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "ovt_aom_require_inspection: '${target}' is not a target.")
  endif()
  if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(FATAL_ERROR "OVT_SYSTEM_LIBAOM=ON: the CONFIG_INSPECTION check runs a probe program, which a cross "
                        "build cannot do. Use the bundled libaom (OVT_SYSTEM_LIBAOM=OFF).")
  endif()
  set(probe
      [=[
#include <aom/aom_decoder.h>
#include <aom/aomdx.h>
#include <stdio.h>

static void on_frame(void *decoder, void *ctx) {
  (void)decoder;
  (void)ctx;
}

int main(void) {
  aom_codec_ctx_t codec;
  aom_codec_dec_cfg_t cfg = {0};
  if (aom_codec_dec_init(&codec, aom_codec_av1_dx(), &cfg, 0) != AOM_CODEC_OK) {
    puts("aom_codec_dec_init failed");
    return 2;
  }
  aom_inspect_init init = {on_frame, NULL};
  aom_codec_err_t err = aom_codec_control(&codec, AV1_SET_INSPECTION_CALLBACK, &init);
  aom_codec_destroy(&codec);
  if (err != AOM_CODEC_OK) {
    printf("AV1_SET_INSPECTION_CALLBACK returned %d\n", (int)err);
    return 1;
  }
  return 0;
}
]=])
  try_run(run_rc compile_ok SOURCE_FROM_CONTENT ovt_aom_inspection_probe.c "${probe}" LINK_LIBRARIES ${target}
          COMPILE_OUTPUT_VARIABLE compile_out RUN_OUTPUT_VARIABLE run_out NO_CACHE)
  if(NOT compile_ok)
    message(FATAL_ERROR "libaom was not built with CONFIG_INSPECTION: the probe against <aom/aomdx.h> did not "
                        "compile or link with ${target}:\n${compile_out}")
  endif()
  if(NOT run_rc EQUAL 0)
    string(STRIP "${run_out}" run_out)
    message(
      FATAL_ERROR "libaom was not built with CONFIG_INSPECTION (${run_out}). Build it with "
                  "-DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1, or use the bundled libaom " "(OVT_SYSTEM_LIBAOM=OFF).")
  endif()
  message(STATUS "OVT: libaom of ${target} has CONFIG_INSPECTION")
endfunction()
