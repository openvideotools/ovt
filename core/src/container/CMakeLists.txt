# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `container` module of libovt-core (chapter 0 §0.3, chapter 08, WI-1.200): format sniffer, CNT-*
# diagnostic registry and the ovt_container_* entry points. Sources go into ovt_core_objects, which
# ovt_core absorbs. Per-format readers (WI-1.201 onwards) add their sources here.

target_sources(
  ovt_core_objects
  PRIVATE annexb.cpp
          byte_service.cpp
          container.cpp
          isobmff/box_registry.cpp
          isobmff/box_registry.gen.cpp
          isobmff/box_walk.cpp
          isobmff/decoders/fragment_boxes.cpp
          isobmff/decoders/codec_config.cpp
          isobmff/decoders/colour_hdr.cpp
          isobmff/decoders/movie.cpp
          isobmff/decoders/sample_entries.cpp
          isobmff/decoders/sample_tables.cpp
          isobmff/field_builder.cpp
          isobmff/fragments.cpp
          isobmff/hdr_summary.cpp
          isobmff/lazy.cpp
          isobmff/paged_span.cpp
          isobmff/tracks.cpp
          isobmff/validate.cpp
          diagnostics.cpp
          format_sniff.cpp
          ivf.cpp
          mpegts/descriptors.cpp
          mpegts/pes.cpp
          mpegts/psi.cpp
          mpegts/timing.cpp
          mpegts/tr101290.cpp
          mpegts/ts_packets.cpp
          mxf_placeholder.cpp
          obu.cpp
          page_cache.cpp
          pix_fmt_grammar.cpp
          rawyuv.cpp
          virtual_file/m3u8.cpp
          virtual_file/mpd.cpp
          y4m.cpp)
# ovt_schemas orders the build after flatc and puts ovt_container_generated.h on the include path.
target_link_libraries(ovt_core_objects PRIVATE ovt_schemas)
# nlohmann_json parses the raw-YUV sidecar (WI-1.229); header-only, an implementation detail.
target_link_libraries(ovt_core_objects PRIVATE "$<BUILD_INTERFACE:${nlohmann_json_TARGET}>")
# The DASH MPD parser (WI-4.208) reads XML with pugixml (MIT); the target is found in core/CMakeLists.txt.
target_link_libraries(ovt_core_objects PRIVATE "$<BUILD_INTERFACE:${pugixml_TARGET}>")
