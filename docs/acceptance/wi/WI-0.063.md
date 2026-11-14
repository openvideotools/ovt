# WI-0.063 acceptance record

Spec version: 1.0-rc24. The automated part was written and run on 2026-11-14 in a Linux cloud container (no macOS,
no `lipo`, no `codesign`). The macOS part ran the same day on the project's Mac Mini (Apple silicon, Rosetta 2); its
output is below.

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

On a Mac (real `lipo`, real `codesign`). Both bundles must come from the same commit and the same Qt. The
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
# every bundled plugin dlopens in both slices (`ovt plugins` does not exist yet, CLI-0001):
for a in arm64 x86_64; do for p in "build/universal/OVT Studio.app/Contents/PlugIns/ovt/"*.dylib; do
  arch -$a /usr/bin/python3 -c 'import ctypes, sys; ctypes.CDLL(sys.argv[1])' "$p" && echo "$a ok $(basename "$p")"
done; done
```

### Record: 2026-11-14, Mac Mini (Apple silicon, macOS, Rosetta 2)

Run by the project's Mac Mini session on this PR at 14f1064 merged locally with the bundling fixes of #464 at 5c0b221
(local merge commit `af2fe00`). Both slices were built with the `macos-arm64-release` and `macos-x86_64-release`
presets and bundled with `tools/mac/bundle.sh --skip-notices-build`; libplacebo's x86_64 build needed
`-DOVT_MESON=<repo>/.venv/bin/meson` on that machine. Output:

```console
make_universal.py: build/universal/OVT Studio.app: 115 Mach-O files merged (x86_64 arm64), signed ad hoc
make_universal rc=0
Mach-O files: 115, without both slices: 0
Architectures in the fat file: build/universal/OVT Studio.app/Contents/MacOS/ovt are: x86_64 arm64
Architectures in the fat file: build/universal/OVT Studio.app/Contents/MacOS/OVT Studio are: x86_64 arm64
codesign rc=0
-- arch arm64
{"ovt":"0.1.0","core":"0.1.0","api":"1.0.0","build":{"commit":"af2fe00","profile":"relwithdebinfo","platform":"macos-arm64"},"features":[]}
ovt rc=0
core 0.1.0
Qt 6.8.3 (built with 6.8.3)
studio rc=0
-- arch x86_64
{"ovt":"0.1.0","core":"0.1.0","api":"1.0.0","build":{"commit":"af2fe00","profile":"relwithdebinfo","platform":"macos-x86_64"},"features":[]}
ovt rc=0
core 0.1.0
Qt 6.8.3 (built with 6.8.3)
studio rc=0
```

The Mini session also reported that all 4 bundled plugins `dlopen` under both `arch -arm64` and `arch -x86_64`, and
that two `--no-sign` merges were byte-identical (determinism). Those lines are not in the log above. A first attempt
at the plugin check through `ovt plugins` failed with `CLI-0001 unknown subcommand 'plugins'` (that subcommand comes
later), so the record lists the `dlopen` loop instead.

Result: test 1 passes. Every Mach-O reports `x86_64 arm64`, and the universal app launches on both architectures.

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
