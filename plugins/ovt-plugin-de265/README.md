<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# ovt-plugin-de265

The HEVC analysis plugin: `libovt_plugin_de265.{so,dylib}`. Specified in `docs/spec/07-bitstream-and-decoders.md`
§7.6.2. Licence LGPL-3.0-or-later.

- **Upstream:** libde265 1.0.15, pinned in `third_party/VERSIONS.md`. It is built static and position-independent by
  `third_party/libde265.cmake` with `-DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DBUILD_SHARED_LIBS=OFF
  -DDE265_INTERNALS=ON`, and `-fno-sanitize=shift-base` under UBSan (issue #236).
- **Patches:** `patches/series`. Patch 0001 adds the read-only internals API (WI-2.162). Patch 0003 adds opt-in
  plane snapshots (WI-2.166): pre-deblocking, pre-SAO, prediction and dequantised coefficients.
- **Planes:** `Reconstruction`, and with patch 0003 `Prediction`, `Residual` (int16, `PreDeblock − Prediction`, so
  `Prediction + Residual == PreDeblock` exactly), `PreDeblock`, `PreSaoOrCdef` and `Coefficients` (int32). PCM and
  `cu_transquant_bypass` coding units have prediction 0 by convention, so their residual is the reconstruction.
  libde265 takes the snapshots while a picture decodes and they cost about four pictures' worth of memory, so they
  are off until a `pull()` asks for one of those stages; from then on every picture that starts decoding carries
  them. Frames decoded before that request carry `Reconstruction` only (the host then reports `ANA-0001`).
- **Profiles:** Main, Main 10 and Main Still Picture. Push refuses RExt, SCC and multi-layer streams with
  `OVT_E_UNSUPPORTED` and Diagnostic `DEC-H265-0010 "use ovt-plugin-hm"`.
- **Sources:** `src/plugin.cpp` holds the ABI glue and builds the FrameAnalysis buffers. `src/decoder.cpp` drives
  libde265. `src/planes.cpp` copies the snapshots out and builds the `PlaneRef`s.
- **Tests:** `tests/`. `ovt_plugin_de265_smoke_tests` loads the plugin through the host's plugin registry and checks
  each reconstruction against the stream's decoded picture hash SEI. `ovt_plugin_de265_planes_tests` checks the
  patch-0003 stages against decodes with libde265's in-loop filters switched off.

HEVC is patent-encumbered, so this plugin is not part of the default downloads (`tools/license_policy.toml`
`[distribution]`).
