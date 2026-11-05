# WI-0.603 acceptance record

Date: 2026-11-05. Machine: cloud container, Ubuntu 24.04, CMake 3.28, Python 3.13.

## Test 1: two builds are byte-identical

Real configured tree: `cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DOVT_SYSTEM_DAV1D=ON -DOVT_BUILD_TESTS=OFF`
(system packages from apt, flatc 25.12.19 built from source, GCC 13, Ubuntu 24.04), then:

```sh
cmake --build build/linux-x64-debug --target ovt_notices   # gate PASS (0 findings, 27 components); 8 listed
cp build/linux-x64-debug/THIRD_PARTY_NOTICES.md n1.md
cmake --build build/linux-x64-debug --target ovt_notices
cmp n1.md build/linux-x64-debug/THIRD_PARTY_NOTICES.md      # identical, sha256 db8ba263...c9a5f
```

The licence texts were not vcpkg's: the container cannot run vcpkg, so `VCPKG_INSTALLED_DIR` pointed at a stand-in
`<port>/copyright` tree copied from the Debian packages' `/usr/share/doc/*/copyright` (flatbuffers: its `LICENSE`).
Without it the target fails with `E-NOTICE-MISSING fmt::fmt` and so on, as designed. Re-run on a vcpkg tree (Linux
CI or the Mac Mini) is still needed. With `OVT_BUILD_TESTS=ON` the gate itself fails (`E-TARGET ovt_core_tests`,
`E-UNKNOWN` for the probe targets): the test targets are missing from `[targets]` of `tools/license_policy.toml`,
which is WI-0.602's, not this WI's.

Automated: `tools/tests/test_gen_notices.py::test_two_runs_are_byte_identical`.

## Test 2: missing licence text fails

Automated: `test_missing_licence_text_fails_with_code` (stderr `E-NOTICE-MISSING ONNX Runtime`, exit 1, no output file).

## Test 3: macOS bundle and codec pack

Pending. The bundle (`ovt_studio`) and the codec pack do not exist yet. The generator content is covered by
`test_content` (AV1 and FFmpeg sentences, `PATENT_NOTICE.md` with `--patent-notice`). The install rule for
`Contents/Resources/` is in `cmake/OvtNotices.cmake`. The inspection of the real bundle belongs to the first macOS
bundle build (WI-0.036 release job).
