# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fuzz harness (chapter 14 §14.7, chapter 13 §13.2.1, WI-0.557).
#
#   ovt_add_fuzz_target(<name> <source>)
#       <source> defines `extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t)`. It creates:
#         ovt_fuzz_<name>          the libFuzzer executable, in ${CMAKE_BINARY_DIR}/fuzz/ (so
#                                  build/linux-x64-fuzz/fuzz/ovt_fuzz_<name>). Only with OVT_ENABLE_FUZZ=ON
#                                  on Clang; it links ${OVT_FUZZ_ENGINE} (default -fsanitize=fuzzer).
#         ovt_fuzz_<name>_regress  the same source with the replay main of test_fuzz_regressions.cpp, built on
#                                  every compiler (GCC and Apple Clang have no libFuzzer), plus the ctest entry
#                                  fuzz_<name>_regress (labels fast, fuzz-regress). It replays
#                                  regressions/<name>/* (minimised crash inputs) and corpus/<name>/* (seeds).
#
# SPEC-ASSUMPTION(#90): the replay directory is regressions/<name>/ (name without the fuzz_ prefix), as in §14.7.1.
#
# The targets link the module objects (ovt_core_objects), not ovt::core: ovt_core exports only the C API (#62).
# fuzz_container_sniff is WI-1.200; the other splitter and parser targets (fuzz_container_*, fuzz_syntax_*) are
# added by the work items that own those parsers (WI-1.238 and the syntax WIs).

function(ovt_add_fuzz_target name source)
  set(regress_dir "${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name}")
  set(corpus_dir "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")

  add_executable(ovt_fuzz_${name}_regress "${source}" test_fuzz_regressions.cpp)
  target_link_libraries(ovt_fuzz_${name}_regress PRIVATE ovt_core_objects ovt_schemas ${flatbuffers_TARGET}
                                                         ${pugixml_TARGET} ovt::flags)
  target_include_directories(ovt_fuzz_${name}_regress PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
  ovt_set_license(ovt_fuzz_${name}_regress "LGPL-2.1-or-later")
  add_test(NAME fuzz_${name}_regress COMMAND ovt_fuzz_${name}_regress "${regress_dir}" "${corpus_dir}")
  set_tests_properties(fuzz_${name}_regress PROPERTIES LABELS "fast;fuzz-regress")

  if(OVT_ENABLE_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(ovt_fuzz_${name} "${source}")
    target_link_libraries(ovt_fuzz_${name} PRIVATE ovt_core_objects ovt_schemas ${flatbuffers_TARGET} ${pugixml_TARGET}
                                                   ovt::flags ${OVT_FUZZ_ENGINE})
    target_include_directories(ovt_fuzz_${name} PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
    set_target_properties(ovt_fuzz_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
    ovt_set_license(ovt_fuzz_${name} "LGPL-2.1-or-later")
  endif()
endfunction()

ovt_add_fuzz_target(annexb_split fuzz_annexb_split.cpp)
ovt_add_fuzz_target(bitreader fuzz_bitreader.cpp)
ovt_add_fuzz_target(container_sniff fuzz_container_sniff.cpp)
ovt_add_fuzz_target(ivf fuzz_ivf.cpp)
ovt_add_fuzz_target(mpd fuzz_mpd.cpp) # WI-4.208: the DASH MPD parser
ovt_add_fuzz_target(mpegts fuzz_mpegts.cpp)
ovt_add_fuzz_target(obu_split fuzz_obu_split.cpp)
ovt_add_fuzz_target(rbsp fuzz_rbsp.cpp)
ovt_add_fuzz_target(y4m fuzz_y4m.cpp)
ovt_add_fuzz_target(unit_scan fuzz_unit_scan.cpp)
ovt_add_fuzz_target(user_data fuzz_user_data.cpp)
# WI-0.091: the cache record reader (zstd frame + FlatBuffer verify) and the meta.json parser.
ovt_add_fuzz_target(cache_blob fuzz_cache_blob.cpp)
ovt_add_fuzz_target(cache_meta fuzz_cache_meta.cpp)
# WI-1.230: the libavformat Demuxer over the custom AVIOContext (byte 0 picks the hint_format).
ovt_add_fuzz_target(demuxer_open fuzz_demuxer_open.cpp)
ovt_add_fuzz_target(yuview_csv fuzz_yuview_csv.cpp) # WI-0.089: the YUView CSV importer
# WI-1.116: the AV1 OBU header, sequence header, metadata, tile list, padding and temporal delimiter parsers.
ovt_add_fuzz_target(syntax_av1 fuzz_syntax_av1.cpp)
# WI-1.112: H.265 header parsers (the selector byte grows with each part of the work item).
ovt_add_fuzz_target(h265_ps fuzz_h265_ps.cpp)
# WI-1.113: H.265 slice_segment_header(), ref_pic_lists_modification(), pred_weight_table() and entry points.
ovt_add_fuzz_target(h265_slice fuzz_h265_slice.cpp)
# WI-3.300: the two JSON option parsers of the metrics module (chapter 14 §14.1.2 item 3).
ovt_add_fuzz_target(normalization_config fuzz_normalization_config.cpp)
ovt_add_fuzz_target(metrics_jobspec fuzz_metrics_jobspec.cpp)
# WI-1.231: the unit splitter of §8.7.4 over every framing.
ovt_add_fuzz_target(unit_split fuzz_unit_split.cpp)
# WI-1.119: AV1 frame-header peek, frame / temporal-unit grouping and the slot model over a Section 5 OBU stream.
ovt_add_fuzz_target(av1_frame_boundary fuzz_av1_frame_boundary.cpp)
