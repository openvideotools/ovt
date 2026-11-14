# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# RPM Fusion "freeworld" codec plugins for OVT (chapter 13 section 13.6.3, ADR-015, WI-0.072).
#
# *** PATENT NOTICE. DO NOT SUBMIT THIS SPEC TO RPM FUSION OR ANY OTHER REPOSITORY WITHOUT KARL'S LEGAL CHECK. ***
# This package enables decoding of patent-encumbered codecs: H.264/AVC and H.265/HEVC (MPEG LA / Access Advance /
# Via LA pools), plus whatever else RPM Fusion's full FFmpeg enables (AAC, MPEG-4 Part 2, VC-1 and others).
# Writing the spec is allowed; distributing it or its binaries is not decided. See docs/dev/packaging.md.
#
# Builds ONLY the two plugins (ovt_plugin_ffmpeg against RPM Fusion's ffmpeg, ovt_plugin_de265) and installs
# the plugins component. The rest of the tree is compiled as a build dependency and discarded, so the core must
# match the `ovt` package of the same version.
# Not part of CI. Build locally in mock with the RPM Fusion repositories enabled:
#   mock -r fedora-41-x86_64-rpmfusion_free packaging/rpm/ovt-codecs-freeworld.spec  (see docs/dev/packaging.md)

%global ovt_preset linux-x64-distro

Name:           ovt-codecs-freeworld
Version:        0.1.0
Release:        0.1%{?dist}
Summary:        AVC/HEVC decoder plugins for OVT (RPM Fusion freeworld)
License:        LGPL-2.1-or-later AND LGPL-3.0-or-later
URL:            https://github.com/openvideotools/ovt
Source0:        %{url}/archive/v%{version}/ovt-%{version}.tar.gz

# Same name FR-LNX-004 uses for the package that carries the AVC/HEVC plugins.
Provides:       ovt-plugins-freeworld = %{version}-%{release}
Requires:       ovt-libs%{?_isa} = %{version}
# RPM Fusion's FFmpeg replaces Fedora's ffmpeg-free; this is the point of the freeworld flavour.
BuildRequires:  ffmpeg-devel
Requires:       ffmpeg-libs%{?_isa}
BuildRequires:  cmake >= 3.28
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  nasm
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(dav1d)
BuildRequires:  pkgconfig(libplacebo)
BuildRequires:  pkgconfig(libvmaf)
BuildRequires:  pkgconfig(zimg)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  flatbuffers-devel
BuildRequires:  flatbuffers-compiler
BuildRequires:  zstd-devel
BuildRequires:  python3-devel
BuildRequires:  libde265-devel
ExclusiveArch:  x86_64

%description
Decoder plugins that let OVT analyse H.264/AVC and H.265/HEVC streams: the
FFmpeg plugin (built against RPM Fusion's full FFmpeg) and the libde265 HEVC
analysis plugin. These codecs are patent-encumbered, which is why they are not
in Fedora's own ovt package.

%prep
%autosetup -n ovt-%{version}

%build
# Only the plugins matter; everything else is off to keep the build short.
cmake --preset %{ovt_preset} \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_INSTALL_LIBDIR=%{_lib} \
  -DOVT_PATENT_CODECS_IN_CORE=OFF \
  -DOVT_BUILD_PLUGIN_AOM=OFF \
  -DOVT_BUILD_PLUGIN_FFMPEG=ON \
  -DOVT_BUILD_PLUGIN_DE265=ON \
  -DOVT_BUILD_GUI=OFF \
  -DOVT_BUILD_CLI=OFF \
  -DOVT_BUILD_RENDER=OFF \
  -DOVT_BUILD_TESTS=OFF \
  -DOVT_INSTALL_DEV=OFF
cmake --build --preset %{ovt_preset} --parallel %{?_smp_build_ncpus}

%install
DESTDIR=%{buildroot} cmake --install build/%{ovt_preset} --component plugins

%files
%license LICENSES/LGPL-2.1-or-later.txt LICENSES/LGPL-3.0-or-later.txt
%{_libdir}/ovt/plugins/libovt_plugin_ffmpeg.so
%{_libdir}/ovt/plugins/libovt_plugin_de265.so

%changelog
* Mon Nov 09 2026 Open Video Tools contributors <packaging@openvideotools.invalid> - 0.1.0-0.1
- Packaging skeleton (WI-0.072); not submitted anywhere
