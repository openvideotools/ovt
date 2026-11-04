# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FlatBuffers schemas of libovt-core (chapter 6 §6.3, chapter 13 WI-0.003). The schemas are source files:
# generated code lands below ${OVT_GENERATED_DIR} (= build/<preset>/generated/, chapter 13 §13.2.3) and is
# never committed. Every core/schema/*.fbs is picked up, so a new schema file (WI-0.080, WI-0.081, ...)
# needs no edit here. WI-0.083 extends ovt_add_flatbuffers_schema (.bfbs embedding, schema_version.h).

file(GLOB _ovt_schema_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.fbs")
ovt_add_flatbuffers_schema(ovt_schemas ${_ovt_schema_files})
unset(_ovt_schema_files)
