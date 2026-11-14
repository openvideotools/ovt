# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fedora spec for the patent-free OVT build (chapter 13 section 13.6.3, WI-0.072). SKELETON: not submitted to Fedora.
# Built against Fedora's FFmpeg (ffmpeg-free), so AVC/HEVC decoding is unavailable. The FFmpeg and de265 plugins
# are left out; they ship as ovt-codecs-freeworld from RPM Fusion (ovt-codecs-freeworld.spec).
# Check with:  rpmbuild -ba packaging/rpm/ovt.spec   (Fedora container; docs/dev/packaging.md)

%global ovt_preset linux-x64-distro

Name:           ovt
Version:        0.1.0
Release:        0.1%{?dist}
Summary:        Open Video Tools: video bitstream, container and quality analysis
# Per-directory licences, tools/license_policy.toml. aom plugin: BSD-2-Clause plus the AOMedia patent licence.
License:        LGPL-2.1-or-later AND GPL-3.0-or-later AND BSD-2-Clause AND ISC
URL:            https://github.com/openvideotools/ovt
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
# libaom (static, CONFIG_INSPECTION=1) is a second source because Fedora's libaom has no inspection support.
# Bundled-library declaration required by the Fedora packaging guidelines:
Provides:       bundled(libaom) = 3.12.1

ExclusiveArch:  x86_64

BuildRequires:  cmake >= 3.28
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  nasm
BuildRequires:  pkgconfig
BuildRequires:  python3-devel
BuildRequires:  pybind11-devel
BuildRequires:  ffmpeg-free-devel
BuildRequires:  pkgconfig(dav1d)
BuildRequires:  pkgconfig(libplacebo)
BuildRequires:  pkgconfig(libvmaf)
BuildRequires:  pkgconfig(zimg)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  flatbuffers-devel
BuildRequires:  flatbuffers-compiler
BuildRequires:  zstd-devel
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qttools-devel
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description
OVT (Open Video Tools) analyses H.264, H.265 and AV1 bitstreams, containers and
video quality. This package has the ovt command-line tool and OVT Studio, the
graphical analyzer, with AV1 analysis through libaom and dav1d.

This build does not decode AVC or HEVC. Those codecs come from the
ovt-codecs-freeworld package in RPM Fusion.

%package        libs
Summary:        Core and rendering libraries for OVT

%description    libs
Shared libraries libovt_core and libovt_render, and the AV1 analysis plugin.

%package        devel
Summary:        Development files for OVT
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description    devel
C API headers, ovt-config.cmake and the ovt-core pkg-config file.

%package -n python3-%{name}
Summary:        Python bindings for OVT
Requires:       %{name}-libs%{?_isa} = %{version}-%{release}

%description -n python3-%{name}
Python 3 module (ovt_py) exposing the OVT analysis API.

%prep
%autosetup -n %{name}-%{version}

%build
# AVC/HEVC decoding stays out of this package: no FFmpeg plugin, no de265 plugin, and the preset's
# OVT_PATENT_CODECS_IN_CORE=ON (right for Debian and Arch) is switched off for Fedora.
cmake --preset %{ovt_preset} \
  -DCMAKE_INSTALL_PREFIX=%{_prefix} \
  -DCMAKE_INSTALL_LIBDIR=%{_lib} \
  -DOVT_PATENT_CODECS_IN_CORE=OFF \
  -DOVT_BUILD_PLUGIN_FFMPEG=OFF \
  -DOVT_BUILD_PLUGIN_DE265=OFF \
  -DOVT_BUILD_PYTHON=ON \
  -DOVT_BUILD_TESTS=OFF \
  -DOVT_NOTICES_REQUIRED=ON
cmake --build --preset %{ovt_preset} --parallel %{?_smp_build_ncpus}

%install
DESTDIR=%{buildroot} cmake --install build/%{ovt_preset}

%check
# Unit tests run in upstream CI. The distro check is that the patent-free build really lacks the FFmpeg plugin.
test ! -e %{buildroot}%{_libdir}/ovt/plugins/libovt_plugin_ffmpeg.so
test ! -e %{buildroot}%{_libdir}/ovt/plugins/libovt_plugin_de265.so
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/*.metainfo.xml

%files
%license LICENSES/*
%doc README.md
%{_bindir}/ovt
%{_bindir}/ovt-studio
%{_datadir}/applications/*.desktop
%{_metainfodir}/*.metainfo.xml
%{_datadir}/icons/hicolor/
%{_datadir}/doc/ovt/

%files libs
%{_libdir}/libovt_core.so.*
%{_libdir}/libovt_render.so.*
%dir %{_libdir}/ovt
%dir %{_libdir}/ovt/plugins
%{_libdir}/ovt/plugins/libovt_plugin_aom.so
%{_datadir}/ovt/

%files devel
%{_includedir}/ovt/
%{_libdir}/libovt_core.so
%{_libdir}/libovt_render.so
%{_libdir}/cmake/ovt/
%{_libdir}/pkgconfig/ovt-core.pc

%files -n python3-%{name}
%{python3_sitearch}/ovt_py*

%changelog
* Mon Nov 09 2026 Open Video Tools contributors <packaging@openvideotools.invalid> - 0.1.0-0.1
- Packaging skeleton (WI-0.072); not submitted to Fedora
