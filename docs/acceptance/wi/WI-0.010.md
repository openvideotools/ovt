# WI-0.010 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Python 3.13, pytest 8.3.4.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.010 pull request.

Automated tests: `tools/tests/test_third_party_tools.py` (20 tests). They run in the `tools_pytest` CTest
test (label `fast`) once WI-0.550 is merged, and with `python -m pytest tools/tests` before that.

## Network limits of this machine

The container reaches storage.googleapis.com and GitHub release downloads. It is refused (HTTP 403) by
ffmpeg.org, code.videolan.org and GitHub archive downloads. `tools/versions_to_cmake.py --update-hashes` therefore
filled the hashes of libaom (and its `PATENTS sha256=` note), libde265 and MoltenVK. The FFmpeg, dav1d,
libvmaf and libplacebo rows still hold `<sha>`.

**To do on a machine with full network access** (the Mac Mini, or any developer machine): run
`python3 -I tools/versions_to_cmake.py --update-hashes` and commit `third_party/VERSIONS.md`. It must happen
before WI-0.011 (dav1d) and WI-0.012 (FFmpeg) need those rows; `ovt_tp_declare()` refuses a row without a
hash.

## Test 1: configure writes `versions.cmake` with the `OVT_TP_` spelling only

```console
$ cmake --preset linux-x64-debug -DCMAKE_TOOLCHAIN_FILE= -DOVT_USE_VCPKG=OFF
...
-- Build files have been written to: /home/claude/ovt/build/linux-x64-debug
$ grep -E 'OVT_TP_libaom_(VERSION|URL|SHA256)' build/linux-x64-debug/third_party/versions.cmake
set(OVT_TP_libaom_VERSION "3.12.1")
set(OVT_TP_libaom_URL "https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz")
set(OVT_TP_libaom_SHA256 "9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e")
$ grep -P -c '\bOVT_(?!TP_)[A-Z0-9]+_(VERSION|URL|SHA256)\b' build/linux-x64-debug/third_party/versions.cmake
0
```

Automated: `test_repository_table_emits_the_tp_spelling_only`, `test_configure_downloads_and_verifies`.
Result: pass.

## Test 2: a row whose SHA-256 does not match fails configure and names the row

A fixture project includes `OvtThirdPartyCommon` and calls `ovt_tp_declare(libfixture)` against a
VERSIONS.md whose row points at a local archive (`file://` URL) with a wrong hash:

```console
$ cmake -S p -B b -DOVT_TP_VERSIONS_MD=$PWD/VERSIONS.md -DOVT_TP_DOWNLOADS=$PWD/dl
CMake Error at /home/claude/ovt/cmake/OvtThirdPartyCommon.cmake:152 (message):
  ovt_tp_declare(libfixture): SHA-256 mismatch for
  file:///tmp/.../shafix/up/libfixture-1.2.3.tar.gz:
  got f92f6a7e95d0f6446cc89e2a41fdf4a23073639480ddb25268082ce40507e0e6,
  VERSIONS.md:5 row 'libfixture' pins
  0000000000000000000000000000000000000000000000000000000000000000.
Call Stack (most recent call first):
  CMakeLists.txt:5 (ovt_tp_declare)
$ echo $?
1
```

Automated: `test_configure_fails_naming_the_row_on_sha_mismatch`, plus `test_configure_fails_on_a_row_without_hash`
(placeholder hash) and `test_url_with_another_version_is_rejected` (the pitfall about a URL that embeds a
different version). Result: pass.

## Test 3: `tools/third_party_offline.py --out dl/` downloads and verifies every archive

```console
$ python3 -I tools/third_party_offline.py --out build/third_party/downloads --only libaom libde265 moltenvk
ok        libaom: libaom-3.12.1.tar.gz
ok        libde265: libde265-1.0.15.tar.gz
ok        MoltenVK: MoltenVK-macos.tar
3 of 3 archives verified in build/third_party/downloads
$ python3 -I tools/third_party_offline.py --out build/third_party/downloads; echo $?
error: VERSIONS.md:8 row 'FFmpeg': no SHA-256 recorded (run tools/versions_to_cmake.py --update-hashes)
error: VERSIONS.md:10 row 'dav1d': no SHA-256 recorded (run tools/versions_to_cmake.py --update-hashes)
error: VERSIONS.md:13 row 'libvmaf': no SHA-256 recorded (run tools/versions_to_cmake.py --update-hashes)
error: VERSIONS.md:14 row 'libplacebo': no SHA-256 recorded (run tools/versions_to_cmake.py --update-hashes)
...
3 of 7 archives verified in build/third_party/downloads
1
```

Automated: `test_offline_fetch_verifies_every_archive`, `test_offline_fetch_fails_on_mismatch_and_placeholder`.
Result: pass for every row that has a hash. The full run is pending the `--update-hashes` step above.

## Smoke test of the helpers with a real ExternalProject

A throw-away project (not committed) used the `_template.cmake` pattern for libde265 1.0.15:
`ovt_tp_declare(libde265)`, `ExternalProject_Add(tp_libde265 … PATCH_COMMAND ${OVT_TP_PATCH} <empty dir>
CMAKE_ARGS ${OVT_TP_CMAKE_TOOLCHAIN_ARGS} …)`, `ovt_tp_import_static(tp::de265 tp_libde265 lib/libde265.a
include)`, and an executable linking `tp::de265`. The archive came from `OVT_TP_DOWNLOADS` without a second
download. The build succeeded, and the executable printed `1.0.15`.

## Patch runner (`tools/apply_patches.py`)

Automated: `test_patches_apply_once_and_are_recorded` (second run is a no-op through the `.ovt-patched`
marker; `patches.json` lists the patch), `test_failing_patch_is_named_and_nothing_is_applied` (`--check`
names the failing patch and leaves the tree untouched), `test_series_order_and_missing_header`,
`test_another_series_on_a_patched_tree_fails`, `test_empty_or_missing_dir_is_a_no_op`, and
`test_source_inside_another_checkout_is_patched` (a source tree under `build/` inside the OVT checkout).
Result: pass.

## Step 5: VERSIONS.md ↔ script consistency

```console
$ python3 -I tools/versions_to_cmake.py --check; echo $?
0
```

Automated: `test_check_flags_a_script_that_spells_its_version`, `test_template_passes_the_script_check`.
WI-0.007 hooks `--check` into pre-commit through `tools/check_versions_md.py`.
