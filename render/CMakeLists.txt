# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libovt-render (chapter 13 §13.2.1: ovt_render, libovt_render.so.1, SHARED, LGPL-2.1-or-later; chapter 10 §10.1.3).
# WI-1.400 creates the library: the public headers of §10.2 and the OffscreenTexture skeleton of VideoSurface on
# libplacebo's Vulkan backend. Later chapter 10 work items add their sources to ovt_render.

add_library(ovt_render SHARED src/color/h273_map.cpp src/surface/video_surface_impl.cpp)
# WI-2.430: OverlayBuilder, CPU side (§10.5.1, §10.5.5).
target_sources(ovt_render PRIVATE src/overlay/builder.cpp src/overlay/heatmap_source.cpp)
add_library(ovt::render ALIAS ovt_render)
# WI-1.401: Picture -> pl_frame upload (§10.3.1).
target_sources(ovt_render PRIVATE src/pipeline/frame_upload.cpp)
# WI-1.403: ViewState -> pl_render_params, target frames, dithering (§10.3.2, §10.3.4, §10.3.5).
target_sources(ovt_render PRIVATE src/pipeline/render_params.cpp)
# WI-1.410: compare modes (§10.2.4, §10.6.4); src/shaders/diff.glsl is embedded by the WI-2.431 loop below.
target_sources(ovt_render PRIVATE src/pipeline/compare.cpp src/pipeline/compare_convert.cpp)
# libplacebo without timestamp queries (MoltenVK query-pool use-after-free, see the source).
target_sources(ovt_render PRIVATE src/surface/vk_no_timestamps.cpp)
# WI-1.404: offscreen interop export, one pl_gpu per §10.2.2 interop row (Metal texture, shared Vulkan device, GL).
target_sources(ovt_render PRIVATE src/platform/offscreen.cpp)
# WI-4.452: HDR measurements (§10.7.1-§10.7.4, §10.7.9). The GLSL of render/src/shaders/ is embedded as string
# constants in a generated header (cmake/OvtEmbedShaders.cmake).
# WI-4.453 adds the false-colour and gamut-clip views (§10.7.5, §10.7.8) on the same Q.
target_sources(ovt_render PRIVATE src/measure/linear_pass.cpp src/measure/reduce.cpp src/measure/histogram.cpp
                                  src/measure/measurer.cpp src/measure/false_color.cpp)
# WI-4.454: waveform, vectorscope and CIE xy scope textures (§10.7.6-§10.7.7).
target_sources(ovt_render PRIVATE src/measure/scopes.cpp src/measure/gamut_tables.cpp)
include(OvtEmbedShaders)
ovt_embed_shaders(
  "${CMAKE_CURRENT_BINARY_DIR}/generated/measure_shaders.h"
  kShaderMeasurePrep
  src/shaders/measure_prep.frag
  kShaderReduce8
  src/shaders/reduce8.frag
  kShaderAccumulate
  src/shaders/accumulate.glsl
  kShaderFalseColor
  src/shaders/false_color.frag)
ovt_embed_shaders("${CMAKE_CURRENT_BINARY_DIR}/generated/scope_shaders.h" kShaderScopes src/shaders/scopes.glsl
                  kShaderScopeClear src/shaders/scope_clear.glsl kShaderScopeResolve src/shaders/scope_resolve.frag)
target_include_directories(ovt_render PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# WI-1.406: NATIVE targets (§10.4.1). The portable rules build everywhere; the macOS CAMetalLayer target is
# Objective-C++ with ARC and links the frameworks it calls (system frameworks, no licence entry).
target_sources(ovt_render PRIVATE src/platform/native_target.cpp)
if(APPLE)
  enable_language(OBJCXX)
  target_sources(ovt_render PRIVATE src/platform/macos_metal.mm)
  set_source_files_properties(src/platform/macos_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(ovt_render PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(ovt_render PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Metal")
endif()

# WI-1.407: display_probe() (§10.4.5). The rules are portable; one platform file queries the screen.
target_sources(ovt_render PRIVATE src/platform/display_caps_rules.cpp)
if(APPLE)
  target_sources(ovt_render PRIVATE src/platform/display_caps_macos.mm)
  set_source_files_properties(src/platform/display_caps_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(ANDROID)
  target_sources(ovt_render PRIVATE src/platform/display_caps_android.cpp)
else()
  target_sources(ovt_render PRIVATE src/platform/display_caps_linux.cpp)
endif()

# WI-2.431: overlay layer, heatmap LUTs (render/luts/, tools/gen_luts.py) and the heatmap/fill/hatch passes (§10.5).
# The GLSL bodies in src/shaders/ are embedded as raw strings in generated headers (shaders/<name>_glsl.h).
target_sources(ovt_render PRIVATE src/overlay/overlay_layer.cpp src/overlay/pass_fill.cpp)
# WI-2.434: MV arrow and intra stroke instances (2.434a) and passes 8-9 by vertex pulling (2.434b, §10.5.4).
target_sources(ovt_render PRIVATE src/overlay/arrow_instances.cpp src/overlay/pass_arrows.cpp)
foreach(shader IN ITEMS arrows diff fill hatch heatmap)
  set(shader_src "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${shader}.glsl")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shader_src}")
  file(READ "${shader_src}" OVT_SHADER_TEXT)
  string(SUBSTRING "${shader}" 0 1 shader_head)
  string(SUBSTRING "${shader}" 1 -1 shader_tail)
  string(TOUPPER "${shader_head}" shader_head)
  set(OVT_SHADER_SYMBOL "k${shader_head}${shader_tail}")
  file(
    CONFIGURE
    OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}_glsl.h"
    CONTENT
    "// Generated by render/CMakeLists.txt from render/src/shaders/${shader}.glsl; do not edit.\n#pragma once\nnamespace ovt::render::shaders {\ninline constexpr const char* @OVT_SHADER_SYMBOL@ = R\"glsl(@OVT_SHADER_TEXT@)glsl\";\n}  // namespace ovt::render::shaders\n"
    @ONLY)
endforeach()
target_include_directories(ovt_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")

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

# WI-1.404: Interop::GlTexture loads GL through EGL and needs GLES 3.0 for its fence; libplacebo has an OpenGL backend
# on Android (and in system packages, which a local OVT_SYSTEM_LIBPLACEBO build may use). Elsewhere the GL row is
# compiled out.
if(ANDROID OR OVT_SYSTEM_LIBPLACEBO)
  find_library(OVT_GLES_LIBRARY NAMES GLESv3 GLESv2)
  find_library(OVT_EGL_LIBRARY NAMES EGL)
  if(OVT_GLES_LIBRARY AND OVT_EGL_LIBRARY)
    target_link_libraries(ovt_render PRIVATE "${OVT_GLES_LIBRARY}" "${OVT_EGL_LIBRARY}")
    target_compile_definitions(ovt_render PRIVATE OVT_RENDER_HAVE_GLES=1)
  endif()
endif()

# SONAME libovt_render.so.1 (chapter 13 §13.2.1).
set_target_properties(ovt_render PROPERTIES OUTPUT_NAME ovt_render EXPORT_NAME render VERSION "1.0.0" SOVERSION "1")
ovt_set_license(ovt_render "LGPL-2.1-or-later")

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
