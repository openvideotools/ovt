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
ovt_add_fuzz_target(canonical fuzz_canonical.cpp) # WI-0.085: canonicalize / structural_diff
ovt_add_fuzz_target(container_sniff fuzz_container_sniff.cpp)
ovt_add_fuzz_target(isobmff_box_decoders fuzz_isobmff_box_decoders.cpp) # WI-1.205: ISOBMFF hand box decoders
ovt_add_fuzz_target(isobmff_tree fuzz_isobmff_tree.cpp) # WI-1.204: ISOBMFF open + tree + expand of every box
ovt_add_fuzz_target(isobmff_fragments fuzz_isobmff_fragments.cpp) # WI-1.219: FragmentTimeline builder
ovt_add_fuzz_target(isobmff_hdr_summary fuzz_isobmff_hdr_summary.cpp) # WI-1.223: HdrMetadata summary and probe
ovt_add_fuzz_target(ivf fuzz_ivf.cpp)
ovt_add_fuzz_target(m3u8 fuzz_m3u8.cpp) # WI-4.209: the HLS M3U8 parser
ovt_add_fuzz_target(mpd fuzz_mpd.cpp) # WI-4.208: the DASH MPD parser
ovt_add_fuzz_target(mpegts fuzz_mpegts.cpp)
ovt_add_fuzz_target(obu_split fuzz_obu_split.cpp)
ovt_add_fuzz_target(rbsp fuzz_rbsp.cpp)
ovt_add_fuzz_target(y4m fuzz_y4m.cpp)
ovt_add_fuzz_target(raw_format_string fuzz_raw_format_string.cpp) # WI-1.229: raw YUV format string, sidecar, heuristics
ovt_add_fuzz_target(unit_scan fuzz_unit_scan.cpp)
ovt_add_fuzz_target(user_data fuzz_user_data.cpp)
# WI-0.091: the cache record reader (zstd frame + FlatBuffer verify) and the meta.json parser.
ovt_add_fuzz_target(cache_blob fuzz_cache_blob.cpp)
ovt_add_fuzz_target(cache_meta fuzz_cache_meta.cpp)
# WI-1.230: the libavformat Demuxer over the custom AVIOContext (byte 0 picks the hint_format).
ovt_add_fuzz_target(demuxer_open fuzz_demuxer_open.cpp)
ovt_add_fuzz_target(yuview_csv fuzz_yuview_csv.cpp) # WI-0.089: the YUView CSV importer
# WI-0.088: the JSON importer (untrusted text) and the JSON exporter (untrusted stored records).
ovt_add_fuzz_target(json_import fuzz_json_import.cpp)
ovt_add_fuzz_target(json_export fuzz_json_export.cpp)
# WI-1.116: the AV1 OBU header, sequence header, metadata, tile list, padding and temporal delimiter parsers.
ovt_add_fuzz_target(syntax_av1 fuzz_syntax_av1.cpp)
# WI-1.117: the AV1 uncompressed_header() sub-structure parsers (part a: frame size, tile_info); the selector byte
# grows with parts b (quantization, segmentation, delta q / lf) and c (the whole header).
ovt_add_fuzz_target(syntax_av1_frame_header fuzz_syntax_av1_frame_header.cpp)
# WI-1.118: the uncompressed_header() rows 20–27 parsers (loop filter … film grain) and tile_group_obu(), in order.
ovt_add_fuzz_target(syntax_av1_frame_header2 fuzz_syntax_av1_frame_header2.cpp)
# WI-1.504a: ovt_syntax_trace (verifier on raw bytes; rebuilt bits on SyntaxBuilder trees).
ovt_add_fuzz_target(syntax_trace fuzz_syntax_trace.cpp)
# WI-1.112: H.265 header parsers (the selector byte grows with each part of the work item).
ovt_add_fuzz_target(h265_ps fuzz_h265_ps.cpp)
# WI-1.113: H.265 slice_segment_header(), ref_pic_lists_modification(), pred_weight_table() and entry points.
ovt_add_fuzz_target(h265_slice fuzz_h265_slice.cpp)
# WI-1.114: the H.265 SEI framing and payload parsers.
ovt_add_fuzz_target(h265_sei fuzz_h265_sei.cpp)
# WI-1.112 follow-up: the H.265 AUD, end of sequence / bitstream and filler data parsers (byte 0 selects the parser).
ovt_add_fuzz_target(h265_aud fuzz_h265_aud.cpp)
# WI-3.300: the two JSON option parsers of the metrics module (chapter 14 §14.1.2 item 3).
ovt_add_fuzz_target(normalization_config fuzz_normalization_config.cpp)
ovt_add_fuzz_target(metrics_jobspec fuzz_metrics_jobspec.cpp)
# WI-1.231: the unit splitter of §8.7.4 over every framing.
ovt_add_fuzz_target(unit_split fuzz_unit_split.cpp)
# WI-0.094: the FrameAnalysis and StreamIndex readers (Verifier + validator).
ovt_add_fuzz_target(frame_analysis fuzz_frame_analysis.cpp)
ovt_add_fuzz_target(stream_index fuzz_stream_index.cpp)
# WI-1.108: H.264 NAL header, SPS and PPS parsers (byte 0 selects the mode).
ovt_add_fuzz_target(h264_ps fuzz_h264_ps.cpp)
# WI-1.109a/b: H.264 slice header, AUD, end of sequence / stream, filler data and data partitions.
ovt_add_fuzz_target(h264_slice fuzz_h264_slice.cpp)
# WI-1.110: the H.264 SEI framing and payload parsers.
ovt_add_fuzz_target(h264_sei fuzz_h264_sei.cpp)
# WI-1.111: the H.264 index chain (ActiveSets with the PPS re-parse, frame boundary, POC) over Annex B bytes.
ovt_add_fuzz_target(h264_index fuzz_h264_index.cpp)

# WI-2.583: push() of ovt-plugin-ffmpeg (chapter 14 §14.7.1). The source lives with the plugin; the target loads the built
# plugin module from its output directory, so it follows the plugin's own option.
if(OVT_BUILD_PLUGIN_FFMPEG)
  set(_ffmpeg_plugin_tests "${PROJECT_SOURCE_DIR}/plugins/ovt-plugin-ffmpeg/tests")
  ovt_add_fuzz_target(plugin_push_ffmpeg "${_ffmpeg_plugin_tests}/fuzz/fuzz_plugin_push_ffmpeg.cpp")
  foreach(_target ovt_fuzz_plugin_push_ffmpeg_regress ovt_fuzz_plugin_push_ffmpeg)
    if(TARGET ${_target})
      target_compile_definitions(${_target} PRIVATE "OVT_FFMPEG_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_ffmpeg>\"")
      add_dependencies(${_target} ovt_plugin_ffmpeg)
    endif()
  endforeach()
endif()

# WI-2.581: push() of ovt-plugin-aom (chapter 14 §14.7.1). Same shape as plugin_push_ffmpeg above; the plugin exists only
# with the bundled libaom (OVT_SYSTEM_LIBAOM=OFF), so the target does too.
if(OVT_BUILD_PLUGIN_AOM AND NOT OVT_SYSTEM_LIBAOM)
  ovt_add_fuzz_target(plugin_push_aom
                      "${PROJECT_SOURCE_DIR}/plugins/ovt-plugin-aom/tests/fuzz/fuzz_plugin_push_aom.cpp")
  foreach(_target ovt_fuzz_plugin_push_aom_regress ovt_fuzz_plugin_push_aom)
    if(TARGET ${_target})
      target_compile_definitions(${_target} PRIVATE "OVT_AOM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_aom>\"")
      add_dependencies(${_target} ovt_plugin_aom)
    endif()
  endforeach()
endif()

# Issue #649 (WI-2.434 follow-up): the overlay builder's CPU side over untrusted FrameAnalysis bytes. The libovt-render
# builder files are compiled in directly (they need FlatBuffers only, no libplacebo), so the target exists in the
# linux-x64-fuzz preset, which sets OVT_BUILD_RENDER=OFF.
set(_render_dir "${PROJECT_SOURCE_DIR}/render")
ovt_add_fuzz_target(overlay_builder fuzz_overlay_builder.cpp)
foreach(_target ovt_fuzz_overlay_builder_regress ovt_fuzz_overlay_builder)
  if(TARGET ${_target})
    target_sources(
      ${_target} PRIVATE "${_render_dir}/src/overlay/builder.cpp" "${_render_dir}/src/overlay/heatmap_source.cpp"
                         "${_render_dir}/src/overlay/arrow_instances.cpp")
    target_include_directories(${_target} PRIVATE "${_render_dir}/src" "${_render_dir}/include")
  endif()
endforeach()
unset(_render_dir)

# WI-1.119: AV1 frame-header peek, frame / temporal-unit grouping and the slot model over a Section 5 OBU stream.
ovt_add_fuzz_target(av1_frame_boundary fuzz_av1_frame_boundary.cpp)

# WI-1.121: the StreamIndex builder end to end (unit scan -> add_unit -> finish -> validate_stream_index).
ovt_add_fuzz_target(index_builder fuzz_index_builder.cpp)
# WI-1.121: the Stream C API (bytes -> ovt_container_open -> ovt_stream_open -> ovt_stream_build_index); WI-1.125
# adds the lazy-tree threshold and ovt_stream_unit_syntax() to it.
ovt_add_fuzz_target(stream_api fuzz_stream_api.cpp)
# WI-1.125: lazy EPB stripping in chunks against rbsp_from_nal().
ovt_add_fuzz_target(lazy_rbsp fuzz_lazy_rbsp.cpp)
# WI-1.129: one unit through every header parser that accepts its type, against fresh canned contexts
# (fuzz_unit_contexts.inc, generated by tools/fuzz_seed.py --contexts). The index target of WI-1.129 is index_builder.
ovt_add_fuzz_target(h264_units fuzz_h264_units.cpp)
ovt_add_fuzz_target(h265_units fuzz_h265_units.cpp)
ovt_add_fuzz_target(av1_obus fuzz_av1_obus.cpp)

# The canary (WI-0.033): inert unless OVT_FUZZ_CANARY is set, then it aborts on its seed. fuzz_canary_trips passes
# only if that replay crashes (expect_crash.cmake; WILL_FAIL cannot invert a signal). Label `canary`: no lane runs it.
ovt_add_fuzz_target(canary fuzz_canary.cpp)
add_test(NAME fuzz_canary_trips
         COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:ovt_fuzz_canary_regress>"
                 "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/corpus/canary" -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_crash.cmake")
set_tests_properties(fuzz_canary_trips PROPERTIES LABELS "canary")
