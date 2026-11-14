# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `container` module of libovt-core (chapter 0 §0.3, chapter 08, WI-1.200): format sniffer, CNT-*
# diagnostic registry and the ovt_container_* entry points. Sources go into ovt_core_objects, which
# ovt_core absorbs. Per-format readers (WI-1.201 onwards) add their sources here.

target_sources(
  ovt_core_objects
  PRIVATE annexb.cpp
          container.cpp
          isobmff/box_walk.cpp
          diagnostics.cpp
          format_sniff.cpp
          ivf.cpp
          mpegts/psi.cpp
          mpegts/ts_packets.cpp
          obu.cpp
          virtual_file/mpd.cpp
          y4m.cpp)
# ovt_schemas orders the build after flatc and puts ovt_container_generated.h on the include path.
target_link_libraries(ovt_core_objects PRIVATE ovt_schemas)
# The DASH MPD parser (WI-4.208) reads XML with pugixml (MIT); the target is found in core/CMakeLists.txt.
target_link_libraries(ovt_core_objects PRIVATE "$<BUILD_INTERFACE:${pugixml_TARGET}>")
