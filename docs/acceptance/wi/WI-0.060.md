# WI-0.060 acceptance record

Spec version: 1.0-rc17. Written 2026-11-09 in a Linux cloud container (no macOS, no `otool`, no Qt), so the
macOS-only steps are **pending**: they run on the Mac Mini and the output is pasted into this file before the PR is
approved. Nothing below has been run on a Mac yet.

## Test 1: only `@rpath`, `@executable_path`, `/usr/lib`, `/System` references (`fix_rpaths.py` exit 0)

Automated part, run here against fake `otool` / `install_name_tool` (the logic of `fix_rpaths.py`, not the Apple
tools): `tools/tests/test_mac_fix_rpaths.py`, 14 tests, run by `ctest --preset pr` through the pytest registration in
`tests/CMakeLists.txt`.

```console
$ pytest tools/tests/test_mac_fix_rpaths.py -q
..............                                                           [100%]
14 passed
```

| Given / When / Then line | Test |
|---|---|
| clean bundle, verify, exit 0 | `test_clean_bundle_passes` |
| a build-tree or Homebrew path is a violation naming the file | `test_absolute_build_tree_reference_fails_and_names_the_file`, `test_homebrew_reference_fails` |
| an `@rpath` reference that resolves to nothing fails | `test_unresolvable_rpath_reference_fails` |
| an `LC_RPATH` outside the bundle fails | `test_rpath_outside_the_bundle_fails` |
| `--fix` repairs plugins, is idempotent, cannot invent a missing library | `test_fix_repairs_plugin_references_and_rpaths`, `test_fix_is_idempotent`, `test_fix_cannot_invent_a_missing_library` |

Pending, on the Mini (real `otool`, real Qt, real `macdeployqt`), as the `ovt-build` user from the repository root:

```sh
tools/bootstrap.sh --no-qt   # only if the toolchain is not there yet
export QT_ROOT_DIR=<Qt version directory from tools/install_qt.py, e.g. $HOME/Qt/ovt/6.8.3, not its macos/ subdirectory>
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
tools/mac/bundle.sh macos-arm64-release      # prints the .app path; exit 0 includes fix_rpaths.py verification
echo "bundle rc=$?"
python3 -I tools/mac/fix_rpaths.py "build/macos-arm64-release/bundle/OVT Studio.app"; echo "verify rc=$?"
# every Mach-O, independently of the script:
find "build/macos-arm64-release/bundle/OVT Studio.app" -type f | while read -r f; do
  file -b "$f" | grep -q Mach-O && otool -L "$f" | tail -n +2 | awk '{print $1}'
done | sort -u | grep -v -E '^(@rpath/|@executable_path/|@loader_path/|/usr/lib/|/System/)' ; echo "stray refs above (none expected)"
codesign --verify --strict "build/macos-arm64-release/bundle/OVT Studio.app"; echo "codesign rc=$?"
ls "build/macos-arm64-release/bundle/OVT Studio.app/Contents/Resources" | head -20   # THIRD_PARTY_NOTICES.md, LICENSES, PATENTS
```

Record: command output, date, machine (to fill in).

First Mini run (f5ba822, macOS 15.8.2 arm64, Qt 6.8.3, 2026-11-08): configure and build exit 0, then `bundle.sh`
stopped in the `ovt_notices` target with `license gate: FAIL (135 findings, 83 components)` (log: the coordinator's
copy of the run, `/mnt/project-files/w060-step1-bundle.log`). Causes found in its `license_manifest.json`: the gate
checked every target of the build tree, including tests and fixtures that are not in the policy `[targets]` table,
and the shipping targets linked items with no licence description (CMake's `::@(0x...)` list markers, the in-tree
`ovt_schemas` and `tp::aom_inspection`, the macOS SDK's `OpenGL.framework`, and libplacebo's glslang and lcms2
static libraries linked by file name). Fixed in the follow-up PR; the steps above are re-run on that head.

Second run for determinism of the script outputs: repeat `bundle.sh` and compare `fix_rpaths.py` stdout; the file
list and the `otool -L` set must be identical (Mach-O bytes carry build timestamps and are not compared).

## Test 2: a clean macOS 13 VM launches the bundle (manual)

Pending; needs a macOS 13 VM, which the Mini does not have. Steps for whoever has one: copy
`OVT Studio.app` (from `build/macos-arm64-release/bundle/`, or the x86_64 one on an Intel VM) to the VM with
`ditto`, `xattr -cr "OVT Studio.app"` (it is ad-hoc signed, not notarised: WI-0.061), open it, and record that the
window appears, `OVT Studio.app/Contents/MacOS/ovt --version` prints, and `ovt plugins` lists `aom`. Record date
and VM image. Until then this line stays open.

## Deviations / assumptions

- The bundle is built from the built `OVT Studio.app` skeleton plus `cmake --install` into `Contents/`, then moved to
  the bundle layout (spec §13.5.2 amended, rc17), because `GNUInstallDirs` names are fixed at configure time.
- `fix_rpaths.py` also accepts `@loader_path/` (macdeployqt uses it for Qt plugins); the spec sentence lists only
  `@rpath`/`@executable_path`.
- `bundle.sh` ad-hoc signs at the end because an arm64 binary will not run unsigned after `install_name_tool`/`strip`.
  Real signing, entitlements and notarisation stay with WI-0.061.
- Not done here: the bundle icon is generated only if `rsvg-convert` and `iconutil` exist; the Mini may lack
  `rsvg-convert`. The Mini record says whether the icon appeared.
