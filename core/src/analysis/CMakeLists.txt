# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `analysis` module of libovt-core (chapter 0 §0.3, chapter 7 §7.5). WI-2.150: the plugin registry and the
# per-platform default plugin directories; WI-2.151: the GOP-local analysis Session (session.cpp). Sources go
# into ovt_core_objects, which ovt_core absorbs.

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_ovt_plugin_paths plugin_paths_android.cpp)
elseif(APPLE)
  set(_ovt_plugin_paths plugin_paths_macos.cpp)
else()
  set(_ovt_plugin_paths plugin_paths_linux.cpp)
endif()
target_sources(ovt_core_objects PRIVATE decoder_plugin_registry.cpp session.cpp ${_ovt_plugin_paths})
# dlopen/dlsym; part of libc on macOS, Android and glibc >= 2.34, libdl before that.
target_link_libraries(ovt_core_objects PUBLIC ${CMAKE_DL_LIBS})
unset(_ovt_plugin_paths)
