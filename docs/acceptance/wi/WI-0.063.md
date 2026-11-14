# WI-0.063 acceptance record

Spec version: 1.0-rc24. Written 2026-11-14 in a Linux cloud container (no macOS, no `lipo`, no `codesign`), so the
macOS-only part is **pending**: it runs on a Mac with Rosetta 2 (the Mac Mini) and the output is pasted into this
file. Nothing below has been run on a Mac yet.

## Test 1: every Mach-O reports `x86_64 arm64`, and the universal app launches on both architectures

Automated part, run here against a fake `lipo` and `codesign` (the logic of `make_universal.py`, not the Apple
tools): `tools/tests/test_mac_make_universal.py`, 12 tests, run by `ctest --preset pr` through the pytest
registration in `tests/CMakeLists.txt`.

```console
$ python3 -m pytest -q -p no:cacheprovider tools/tests/test_mac_make_universal.py
............                                                             [100%]
12 passed
```

| Part of the Given / When / Then line | Test |
|---|---|
| two per-architecture bundles from the same commit merge; every Mach-O reports `x86_64 arm64` | `test_every_macho_is_universal` |
| each slice comes from the bundle of its architecture; already-universal Qt is thinned first | `test_slices_come_from_the_matching_bundle` |
| framework symlinks and resources survive | `test_symlinks_and_resources_are_kept` |
| the merged bundle is re-signed inside-out (ad hoc by default; hardened runtime and entitlements with an identity) | `test_signing_is_inside_out`, `test_real_identity_uses_hardened_runtime_and_entitlements`, `test_no_sign` |
| bundles from different commits or Qt versions are refused and no output is left | `test_differing_resource_fails_and_leaves_no_output`, `test_file_in_one_bundle_only_fails`, `test_wrong_architecture_input_fails`, `test_allow_differing_keeps_the_arm64_copy` |
| usage and tool errors exit 2 | `test_existing_output_and_non_bundle_are_usage_errors`, `test_tool_failure_is_exit_2` |

Pending, on a Mac (real `lipo`, real `codesign`). Both bundles must come from the same commit and the same Qt. The
x86_64 build can run on Apple silicon (cross-compiled with the `macos-x86_64-release` preset), or come from the
`macos-15-intel` CI job's artefact for the same commit:

```sh
cmake --preset macos-arm64-release  && cmake --build --preset macos-arm64-release
cmake --preset macos-x86_64-release && cmake --build --preset macos-x86_64-release
tools/mac/bundle.sh macos-arm64-release
tools/mac/bundle.sh macos-x86_64-release
python3 -I tools/mac/make_universal.py \
  --arm64  "build/macos-arm64-release/bundle/OVT Studio.app" \
  --x86_64 "build/macos-x86_64-release/bundle/OVT Studio.app" \
  --out    "build/universal/OVT Studio.app"; echo "rc=$?"
# every Mach-O, independently of the script:
find "build/universal/OVT Studio.app" -type f | while IFS= read -r f; do
  file -b "$f" | grep -q Mach-O && printf '%s: %s\n' "$(lipo -archs "$f")" "$f"
done | grep -v '^x86_64 arm64: ' ; echo "files without both slices above (none expected)"
codesign --verify --deep --strict "build/universal/OVT Studio.app"; echo "codesign rc=$?"
# launches on both architectures (x86_64 through Rosetta 2):
arch -arm64  "build/universal/OVT Studio.app/Contents/MacOS/ovt" --version
arch -x86_64 "build/universal/OVT Studio.app/Contents/MacOS/ovt" --version
arch -arm64  "build/universal/OVT Studio.app/Contents/MacOS/OVT Studio" --version
arch -x86_64 "build/universal/OVT Studio.app/Contents/MacOS/OVT Studio" --version
arch -x86_64 "build/universal/OVT Studio.app/Contents/MacOS/ovt" plugins   # aom loads in the x86_64 slice
```

Record: command output, date, machine (to fill in).

Determinism: run `make_universal.py` twice into two output paths and compare the file lists and the
`lipo -archs` output. The merged Mach-O bytes are compared only before signing (`--no-sign`), because an ad-hoc
signature is reproducible but a timestamped one is not.

## Deviations / assumptions

- Re-signing (WI step 2) is built into the script instead of calling the WI-0.061 script, because
  `tools/mac/sign_and_notarize.sh` does not exist yet and notarises, which a local merge must not do. The script
  signs inside-out like §13.5.4 steps 1 and 2: ad hoc by default (as `bundle.sh` does), hardened runtime and a
  timestamp with `--identity`, and `tools/mac/entitlements.plist` on the executables and the bundle once WI-0.061
  adds it. `--no-sign` hands the bundle to `sign_and_notarize.sh` unsigned.
- The optional tag job producing `-universal.dmg` (WI step 3) is not added: it needs `make_dmg.sh` and the
  tag-signing secrets from WI-0.061. It belongs in the WI-0.061 tag workflow, or a follow-up once that exists.
- `OVT_UNIVERSAL` stays a configure option with no build effect: a universal build is two builds plus this script
  (§13.5.1), which a single CMake build tree cannot drive.
- The x86_64 slice could not configure libaom on the Mini. Homebrew NASM 3.02 no longer lists `-Ox` in `nasm -hf`,
  which libaom's `test_nasm()` requires ("Unsupported nasm: multipass optimization not supported"). NASM has
  optimised multipass by default since 2.09. A new step in `third_party/libaom.cmake`
  (`third_party/libaom/nasm3_check.cmake`, run between patch and configure) also lets that check accept a NASM whose
  `nasm -v` reports version 3 or later. The object-format checks are unchanged, and NASM 2.x without `-Ox` is still
  refused. This is in this PR because the WI's acceptance run needs an x86_64 bundle; covered by
  `tools/tests/test_libaom_patches.py::test_missing_patch_dir_builds_upstream_and_exports_source_dir`.
- `tools/mac/bundle.sh` runs the licence gate, which currently fails on the shipped targets until PR #202 adds their
  rows. For the acceptance run, `--skip-notices-build` is acceptable; that is #202's fix, not this WI's.
