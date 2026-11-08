# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `demux` module of libovt-core (chapter 0 §0.3, chapter 08 §8.7). Sources go into ovt_core_objects, which
# ovt_core absorbs, like the container module whose splitters it reuses.
#
# WI-1.231: the unit splitter (unit_split.h).

target_sources(ovt_core_objects PRIVATE unit_split.cpp)
# ovt_schemas orders the build after flatc and puts ovt_model_generated.h (fb::DiagnosticT) on the include path.
target_link_libraries(ovt_core_objects PRIVATE ovt_schemas)
