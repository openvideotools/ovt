# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `demux` module of libovt-core (chapter 0 §0.3, chapter 08 §8.7): the libavformat Demuxer over a custom
# AVIOContext (WI-1.230). Sources go into ovt_core_objects, which ovt_core absorbs.

target_sources(ovt_core_objects PRIVATE demuxer.cpp avio_adapter.cpp)
# The core FFmpeg flavour (chapter 13 §13.3.3; the shared libav*_ovt libraries, or the system FFmpeg with
# OVT_SYSTEM_FFMPEG=ON). PUBLIC because demux/demuxer.h includes libavformat headers, so the module tests and
# fuzz targets that link the objects need them too; inside BUILD_INTERFACE so it never reaches an export set
# (core-objects rule 1, #62). The plugin flavour (tp::ffmpeg_plugin) must never be linked next to it.
target_link_libraries(ovt_core_objects PUBLIC "$<BUILD_INTERFACE:tp::ffmpeg_core>" PRIVATE ovt_schemas)
