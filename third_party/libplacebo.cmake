# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libplacebo, the GPU renderer behind OVT's HDR display path (chapter 13 §13.3.1, §13.3.5, chapter 10, WI-0.015).
# Meson wrapper. Produces the imported target tp::placebo (static libplacebo.a, headers under
# include/libplacebo/). The pin (version, URL, SHA-256) is the libplacebo row of third_party/VERSIONS.md.
#
# Options (§13.3.1, names verified against v7.349 meson_options.txt): Vulkan with the proc-address link
# (`vk-proc-addr`: the entry points come from whatever loader the platform provides), OpenGL ES only on Android
# (GLES fallback), glslang as the SPIR-V compiler (shaderc off), lcms2 for ICC profiles, no demos, no tests.
# Optional components that Meson would otherwise pick up from the vcpkg tree (xxhash, libunwind, libdovi,
# Direct3D 11) are switched off, so the library has no dependency this script does not name.
#
# Dependencies, all from the vcpkg tree of the active triplet (or the system with OVT_USE_VCPKG=OFF):
#   * glslang and its SPIRV libraries, and lcms2: linked statically into tp::placebo's interface;
#   * the Vulkan registry vk.xml of `vulkan-headers` (-Dvulkan-registry), which also supplies the Vulkan headers;
#   * a pkg-config module `vulkan`, which libplacebo needs for `vk-proc-addr`: vcpkg's `vulkan-loader` ships
#     one on Linux; on macOS this script writes one that names the prebuilt MoltenVK of third_party/moltenvk.cmake
#     (§13.3.1: direct link, no loader); on Android it names the NDK's system libvulkan.so.
# The generator scripts of libplacebo need python3 Mako and Jinja2 (tools/requirements.txt, `pip install`).
#
# Patches: third_party/patches/libplacebo/0001 (Python 3.13 in utils_gen.py) and 0002 (meson's find_library()
# searches the directories in OVT_TP_LIB_DIRS, the vcpkg tree, for the glslang libraries). The archive is GitLab's
# `-/archive/` tarball, which has no git submodules: Vulkan-Headers (replaced by vulkan-headers above),
# fast_float (the vcpkg port `fast-float`), jinja and markupsafe (the pip packages), and glad (OpenGL loader, Android only; the pip
# package `glad2` provides the same module).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)
include(OvtMeson)

# "./LICENSE", not LICENSE: a bare LICENSE is read as ovt_tp_declare()'s LICENSE keyword and records no file.
ovt_tp_declare(libplacebo LICENSE_FILES ./LICENSE)

# ---- where the vcpkg (or system) dependencies are -----------------------------------------------------
set(_ovt_pl_extra_c "")
set(_ovt_pl_extra_link "")
set(_ovt_pl_pkg_path "")
set(_ovt_pl_hints "")
if(OVT_USE_VCPKG AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  set(_ovt_pl_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  set(_ovt_pl_extra_c "-I${_ovt_pl_root}/include")
  set(_ovt_pl_extra_link "-L${_ovt_pl_root}/lib")
  set(_ovt_pl_pkg_path "${_ovt_pl_root}/lib/pkgconfig")
  set(_ovt_pl_hints HINTS "${_ovt_pl_root}" NO_DEFAULT_PATH)
endif()

find_file(OVT_VULKAN_REGISTRY_XML vk.xml ${_ovt_pl_hints} PATH_SUFFIXES share/vulkan/registry
          DOC "vk.xml of the Vulkan registry (vcpkg port vulkan-headers)")
if(NOT OVT_VULKAN_REGISTRY_XML)
  message(FATAL_ERROR "libplacebo: share/vulkan/registry/vk.xml not found. It comes with the vcpkg port "
                      "`vulkan-headers` (or the distribution package vulkan-headers).")
endif()

# glslang and lcms2 libraries, in link order. The optional SPIRV-Tools pair exists when glslang was built with
# the optimiser (the vcpkg port does).
set(_ovt_pl_libs "")
set(_ovt_pl_libdirs "")
foreach(
  _name
  glslang-default-resource-limits
  glslang
  SPIRV
  MachineIndependent
  OSDependent
  GenericCodeGen
  SPIRV-Tools-opt
  SPIRV-Tools
  lcms2)
  string(MAKE_C_IDENTIFIER "${_name}" _id)
  find_library(OVT_PLACEBO_LIB_${_id} NAMES ${_name} ${_ovt_pl_hints} PATH_SUFFIXES lib)
  if(OVT_PLACEBO_LIB_${_id})
    list(APPEND _ovt_pl_libs "${OVT_PLACEBO_LIB_${_id}}")
    get_filename_component(_ovt_pl_libdir "${OVT_PLACEBO_LIB_${_id}}" DIRECTORY)
    list(APPEND _ovt_pl_libdirs "${_ovt_pl_libdir}")
  elseif(_name MATCHES "^(SPIRV|glslang|lcms2)$")
    message(FATAL_ERROR "libplacebo: library '${_name}' not found (vcpkg ports glslang and lcms; "
                        "OVT_USE_VCPKG=OFF: the system packages).")
  endif()
endforeach()

# meson's find_library() does not search the vcpkg tree; patch 0002 reads these directories from the environment.
list(REMOVE_DUPLICATES _ovt_pl_libdirs)
list(JOIN _ovt_pl_libdirs ":" _ovt_pl_libdirs_env)

# ---- Vulkan: the `vulkan` pkg-config module and the target's link dependency --------------------------
set(_ovt_pl_vulkan_link "")
set(_ovt_pl_depends "")
if(APPLE OR ANDROID_ABI)
  set(_ovt_pl_pc_dir "${CMAKE_BINARY_DIR}/third_party/libplacebo-pkgconfig")
  if(APPLE)
    if(NOT TARGET tp_moltenvk)
      message(FATAL_ERROR "libplacebo: tp_moltenvk is missing; third_party/moltenvk.cmake must be included "
                          "first (cmake/OvtThirdParty.cmake).")
    endif()
    set(_ovt_pl_vulkan_libs "-L${OVT_THIRD_PARTY_PREFIX}/moltenvk/lib -lMoltenVK")
    set(_ovt_pl_depends tp_moltenvk)
    set(_ovt_pl_vulkan_link tp::moltenvk)
  else()
    find_library(OVT_ANDROID_VULKAN_LIB vulkan) # the NDK's sysroot libvulkan.so
    if(NOT OVT_ANDROID_VULKAN_LIB)
      message(FATAL_ERROR "libplacebo: libvulkan.so not found in the Android NDK sysroot.")
    endif()
    set(_ovt_pl_vulkan_libs "${OVT_ANDROID_VULKAN_LIB}")
    set(_ovt_pl_vulkan_link "${OVT_ANDROID_VULKAN_LIB}")
  endif()
  file(WRITE "${_ovt_pl_pc_dir}/vulkan.pc"
       "Name: vulkan\n" "Description: Vulkan implementation OVT links directly (chapter 13 §13.3.1)\n"
       "Version: 1.3.0\n" "Libs: ${_ovt_pl_vulkan_libs}\n" "Cflags: ${_ovt_pl_extra_c}\n")
  list(APPEND _ovt_pl_pkg_path "${_ovt_pl_pc_dir}")
else()
  # Linux: the Vulkan loader. vcpkg's vulkan-loader (static or shared) ships vulkan.pc; with
  # OVT_USE_VCPKG=OFF the system libvulkan-dev does.
  find_library(OVT_VULKAN_LOADER_LIB NAMES vulkan ${_ovt_pl_hints} PATH_SUFFIXES lib)
  if(NOT OVT_VULKAN_LOADER_LIB)
    message(FATAL_ERROR "libplacebo: the Vulkan loader (libvulkan) was not found. It comes with the vcpkg "
                        "port `vulkan-loader` (Linux), or libvulkan-dev with OVT_USE_VCPKG=OFF.")
  endif()
  set(_ovt_pl_vulkan_link "${OVT_VULKAN_LOADER_LIB}")
endif()

if(ANDROID_ABI)
  set(_ovt_pl_gl enabled)
else()
  set(_ovt_pl_gl disabled)
endif()

ovt_tp_apply_patches(_ovt_pl_patch DIR "${CMAKE_CURRENT_LIST_DIR}/patches/libplacebo" INSTALL_DIR
                     "${OVT_THIRD_PARTY_PREFIX}/libplacebo")

ovt_tp_meson(
  libplacebo
  URL
  ${OVT_TP_libplacebo_URL}
  URL_HASH
  SHA256=${OVT_TP_libplacebo_SHA256}
  PATCH_COMMAND
  ${_ovt_pl_patch}
  OPTIONS
  -Db_ndebug=true
  -Dvulkan=enabled
  -Dvk-proc-addr=enabled
  -Dvulkan-registry=${OVT_VULKAN_REGISTRY_XML}
  -Dopengl=${_ovt_pl_gl}
  -Dgl-proc-addr=${_ovt_pl_gl}
  -Dd3d11=disabled
  -Dglslang=enabled
  -Dshaderc=disabled
  -Dlcms=enabled
  -Dlibdovi=disabled
  -Dxxhash=disabled
  -Dunwind=disabled
  -Ddemos=false
  -Dtests=false
  -Dprefer_static=true
  OUTPUT_LIBS
  tp::placebo=lib/libplacebo.a
  DEPENDS
  ${_ovt_pl_depends}
  EXTRA_C_ARGS
  ${_ovt_pl_extra_c}
  EXTRA_LINK_ARGS
  ${_ovt_pl_extra_link}
  PKG_CONFIG_PATH
  ${_ovt_pl_pkg_path}
  ENV
  "OVT_TP_LIB_DIRS=${_ovt_pl_libdirs_env}")

# A changed patch re-extracts the pristine archive (as for libde265), so the patch step never meets a tree
# patched with another set.
file(GLOB _ovt_pl_patch_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/patches/libplacebo/*.patch")
externalproject_add_stepdependencies(tp_libplacebo download ${_ovt_pl_patch_files})

# libplacebo.a is C with C++ parts (glslang); it needs threads, libm and the libraries it was configured with.
find_package(Threads REQUIRED)
set(_ovt_pl_interface Threads::Threads ${_ovt_pl_libs} ${_ovt_pl_vulkan_link})
if(NOT ANDROID_ABI AND NOT APPLE)
  list(APPEND _ovt_pl_interface m)
endif()
set_property(TARGET tp::placebo PROPERTY INTERFACE_LINK_LIBRARIES "${_ovt_pl_interface}")
set_property(TARGET tp::placebo PROPERTY INTERFACE_COMPILE_DEFINITIONS PL_STATIC)
set_property(TARGET tp::placebo PROPERTY IMPORTED_LINK_INTERFACE_LANGUAGES CXX)

unset(_ovt_pl_libdirs_env)
unset(_ovt_pl_libdirs)
unset(_ovt_pl_libdir)
unset(_ovt_pl_extra_c)
unset(_ovt_pl_extra_link)
unset(_ovt_pl_pkg_path)
unset(_ovt_pl_hints)
unset(_ovt_pl_root)
unset(_ovt_pl_libs)
unset(_ovt_pl_vulkan_link)
unset(_ovt_pl_vulkan_libs)
unset(_ovt_pl_depends)
unset(_ovt_pl_pc_dir)
unset(_ovt_pl_gl)
unset(_ovt_pl_patch)
unset(_ovt_pl_patch_files)
unset(_ovt_pl_interface)
