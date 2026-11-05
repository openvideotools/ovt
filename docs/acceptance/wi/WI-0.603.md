# WI-0.603 acceptance record

Date: 2026-11-05. Machine: cloud container, Ubuntu 24.04, CMake 3.28, Python 3.13.

## Test 1: two builds are byte-identical

A stub CMake project that includes `cmake/OvtNotices.cmake` (the full tree cannot configure in this container,
because dav1d's download host is blocked) was built twice:

```sh
cmake -S mini -B b && cmake --build b --target ovt_notices && cp b/THIRD_PARTY_NOTICES.md first.md
cmake --build b --target ovt_notices && cmp first.md b/THIRD_PARTY_NOTICES.md   # no output, exit 0
```

Automated: `tools/tests/test_gen_notices.py::test_two_runs_are_byte_identical`.

## Test 2: missing licence text fails

Automated: `test_missing_licence_text_fails_with_code` (stderr `E-NOTICE-MISSING ONNX Runtime`, exit 1, no output file).

## Test 3: macOS bundle and codec pack

Pending. The bundle (`ovt_studio`) and the codec pack do not exist yet. The generator content is covered by
`test_content` (AV1 and FFmpeg sentences, `PATENT_NOTICE.md` with `--patent-notice`). The install rule for
`Contents/Resources/` is in `cmake/OvtNotices.cmake`. The inspection of the real bundle belongs to the first macOS
bundle build (WI-0.036 release job).
