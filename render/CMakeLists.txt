# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libovt-render (chapter 13 §13.2.1: ovt_render, libovt_render.so.1, SHARED, LGPL-2.1-or-later; chapter 10 §10.1.3).
# WI-1.400 creates the library: the public headers of §10.2 and the OffscreenTexture skeleton of VideoSurface on
# libplacebo's Vulkan backend. Later chapter 10 work items add their sources to ovt_render.

add_library(ovt_render SHARED src/color/h273_map.cpp src/surface/video_surface_impl.cpp)
add_library(ovt::render ALIAS ovt_render)
# WI-1.401: Picture -> pl_frame upload (§10.3.1).
target_sources(ovt_render PRIVATE src/pipeline/frame_upload.cpp)

target_include_directories(
  ovt_render PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The public headers include ovt/color.h (ovt_core) and ovt_model_generated.h (ovt_schemas, build tree only, as for
# the core module objects). libplacebo is a static archive linked privately: its symbols stay hidden inside
# libovt_render (ovt::flags sets -fvisibility=hidden), and tp::placebo carries the Vulkan loader (Linux/Android) or
# MoltenVK (macOS) of WI-0.015/WI-0.017.
target_link_libraries(ovt_render PUBLIC ovt::core "$<BUILD_INTERFACE:ovt_schemas>" PRIVATE tp::placebo ovt::flags)
# WI-1.400 step 1: on macOS ovt_render links the prebuilt MoltenVK of WI-0.017 directly (§13.5.6).
if(TARGET tp::moltenvk)
  target_link_libraries(ovt_render PRIVATE tp::moltenvk)
endif()

# SONAME libovt_render.so.1 (chapter 13 §13.2.1).
set_target_properties(ovt_render PROPERTIES OUTPUT_NAME ovt_render EXPORT_NAME render VERSION "1.0.0" SOVERSION "1")
ovt_set_license(ovt_render "LGPL-2.1-or-later")

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
