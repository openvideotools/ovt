# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# lodepng, vendored (chapter 10 §10.8, WI-1.412): the PNG encoder and decoder of the §10.8 exports. lodepng.cpp and
# lodepng.h are upstream's files with a four-line SPDX marker on top (README.md); the pin is the lodepng row of
# third_party/VERSIONS.md (the version is spelled only there).
#
# Targets:
#   ovt_lodepng_impl  the static library (position independent, hidden visibility, so libovt_render exports none of
#                     its symbols). Built without ovt::flags: upstream code is not held to OVT's -Werror set.
#   lodepng::lodepng  what OVT targets link: an imported interface in front of the library, so the licence manifest
#                     (cmake/OvtLicenseTags.cmake) lists it as a third-party component (Zlib) rather than walking
#                     through it as an OVT target.

include_guard(GLOBAL)

add_library(ovt_lodepng_impl STATIC "${CMAKE_CURRENT_LIST_DIR}/lodepng.cpp")
set_target_properties(ovt_lodepng_impl PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                                                  VISIBILITY_INLINES_HIDDEN ON CXX_CLANG_TIDY "")
# The one PNG feature set OVT uses: encoder, decoder (tests re-read their exports) and the ancillary chunks (cICP,
# mDCV, cLLi). No file I/O helpers (the exporter writes the file itself) and no C++ wrapper.
target_compile_definitions(ovt_lodepng_impl PUBLIC LODEPNG_NO_COMPILE_DISK LODEPNG_NO_COMPILE_CPP)
target_include_directories(ovt_lodepng_impl SYSTEM PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
if(MSVC)
  target_compile_options(ovt_lodepng_impl PRIVATE /w)
else()
  target_compile_options(ovt_lodepng_impl PRIVATE -w)
endif()

add_library(lodepng::lodepng INTERFACE IMPORTED GLOBAL)
set_target_properties(lodepng::lodepng PROPERTIES INTERFACE_LINK_LIBRARIES ovt_lodepng_impl)
ovt_set_dependency_license(lodepng::lodepng SOURCE vendored SPDX "Zlib" LINKAGE static)

# THIRD_PARTY_NOTICES.md (cmake/OvtNotices.cmake): the zlib licence text, staged as notices/licenses/lodepng/LICENSE.
set_property(GLOBAL APPEND PROPERTY OVT_VENDORED_DEPS lodepng)
set_property(GLOBAL PROPERTY OVT_VENDORED_LICENSE_FILES_lodepng "${CMAKE_CURRENT_LIST_DIR}/LICENSE")
