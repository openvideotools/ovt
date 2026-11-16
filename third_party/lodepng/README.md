# third_party/lodepng

[lodepng](https://github.com/lvandeve/lodepng) by Lode Vandevenne, vendored for the PNG exports of chapter 10 §10.8
(WI-1.412): `HdrPng16` (16-bit RGB with the `cICP`, `mDCV` and `cLLi` chunks of PNG Third Edition), `SdrPng8` and
`ViewPng8`. The render tests also decode their exports with it.

- Upstream commit: `7b2893e35891c0c85bee5595caafaf3c2044b409` (version string `20261102`); the pin is the `lodepng` row of
  `third_party/VERSIONS.md`.
- Licence: Zlib (`LICENSE`, upstream's file, unchanged). `tools/license_policy.toml` maps `lodepng.cpp` and `lodepng.h`
  to `Zlib` in `[directories]`.
- Changes from upstream: `lodepng.cpp` and `lodepng.h` each carry four added comment lines at the top (the SPDX
  `FileCopyrightText` and `License-Identifier` lines and a note that marks the files as altered, as the zlib licence
  asks). Nothing else is edited, so `tail -n +5 lodepng.cpp` equals upstream's file byte for byte. The pre-commit
  formatters skip these files.
- Build: `CMakeLists.txt` compiles `lodepng.cpp` into a static, hidden-visibility library without the OVT warning
  flags and puts the imported target `lodepng::lodepng` in front of it for the licence manifest.

To update: copy the two files from the new upstream commit, re-add the four lines, and change the commit here and in
`third_party/VERSIONS.md` in a PR of its own (chapter 13 §13.3.7).
