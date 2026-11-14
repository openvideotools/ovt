<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# ovt-plugin-de265

The HEVC analysis plugin: `libovt_plugin_de265.{so,dylib}`. Specified in `docs/spec/07-bitstream-and-decoders.md`
§7.6.2. Licence LGPL-3.0-or-later.

- **Upstream:** libde265 1.0.15, pinned in `third_party/VERSIONS.md`. It is built static and position-independent by
  `third_party/libde265.cmake` with `-DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DBUILD_SHARED_LIBS=OFF
  -DDE265_INTERNALS=ON`, and `-fno-sanitize=shift-base` under UBSan (issue #236).
- **Patches:** `patches/series`. Patch 0001 adds the read-only internals API (WI-2.162).
- **Profiles:** Main, Main 10 and Main Still Picture. Push refuses RExt, SCC and multi-layer streams with
  `OVT_E_UNSUPPORTED` and Diagnostic `DEC-H265-0010 "use ovt-plugin-hm"`.
- **Sources:** `src/plugin.cpp` holds the ABI glue and builds the FrameAnalysis buffers. `src/decoder.cpp` drives
  libde265.
- **Tests:** `tests/`. `ovt_plugin_de265_smoke_tests` loads the plugin through the host's plugin registry and checks
  each reconstruction against the stream's decoded picture hash SEI.

HEVC is patent-encumbered, so this plugin is not part of the default downloads (`tools/license_policy.toml`
`[distribution]`).
