# Fuzzing and crash triage

OVT reads hostile files, so every parser of untrusted bytes has a libFuzzer target (chapter 14 §14.7). This
page covers running the targets, the OSS-Fuzz onboarding files and the triage procedure for a crash.

## Targets and smoke runs

The targets live in `core/tests/fuzz/fuzz_<name>.cpp` and are registered with `ovt_add_fuzz_target()` in
`core/tests/fuzz/CMakeLists.txt`. With the `linux-x64-fuzz` preset (clang) each becomes
`build/linux-x64-fuzz/fuzz/ovt_fuzz_<name>`. The `fuzz-smoke` CI job runs every target for 60 seconds on each
pull request. Minimised crash inputs live in `core/tests/fuzz/regressions/<name>/` and are replayed on every
platform by `test_fuzz_regressions`, so a fixed bug stays fixed.

## OSS-Fuzz onboarding files

`core/tests/fuzz/oss-fuzz/` holds the three files OSS-Fuzz wants in `projects/ovt/` (§14.7.4):

| File | Purpose |
|---|---|
| `project.yaml` | language `c++`, sanitizers `address` and `undefined`, engine `libfuzzer`, `main_repo` |
| `Dockerfile` | builder image: cmake 3.25 or newer, Ninja, zip, and a clone of this repository with the vcpkg submodule. No Qt |
| `build.sh` | configures with `-DOVT_ENABLE_FUZZ=ON -DOVT_FUZZ_ENGINE="$LIB_FUZZING_ENGINE"`, builds every `ovt_fuzz_*` target, copies the binaries, dictionaries and one `_seed_corpus.zip` per corpus directory to `$OUT` |

There is no `oss-fuzz` CMake preset (§14.7.4, D23). `build.sh` takes `CC`, `CXX`, `CFLAGS` and `CXXFLAGS` from the
OSS-Fuzz environment, so the sanitizer and coverage flags are not set by OVT (`OVT_ENABLE_SANITIZERS=OFF`).
The vcpkg libraries are built from source with the same flags, through a triplet that `build.sh` writes into
`$WORK` (no prebuilt binaries).

MemorySanitizer and the AFL++ and honggfuzz engines are not enabled yet. They need an instrumented libc++ and
a check that the target set links with their drivers. Add them to `project.yaml` in a later change.

### Test it locally

Needs Docker and a checkout of <https://github.com/google/oss-fuzz>.

```sh
cp -r core/tests/fuzz/oss-fuzz/ /path/to/oss-fuzz/projects/ovt/
cd /path/to/oss-fuzz
python infra/helper.py build_image ovt
python infra/helper.py build_fuzzers --sanitizer address --external ovt "$OVT_CHECKOUT"
python infra/helper.py check_build --sanitizer address --external ovt "$OVT_CHECKOUT"
```

Repeat with `--sanitizer undefined`. The WI-0.569 acceptance record goes to
`docs/acceptance/wi/WI-0.569.md` (command, output, date, machine).

### Submitting

Submitting is the owner's step and is not done by an agent. Before the pull request to google/oss-fuzz:

1. Replace the two placeholder contacts in `project.yaml` with the owner and a second maintainer. OSS-Fuzz needs
   addresses that belong to Google accounts, and they become public in the oss-fuzz repository.
2. Check that every target in `core/tests/fuzz/CMakeLists.txt` builds and passes `check_build`.
3. Open the pull request to google/oss-fuzz adding `projects/ovt/` with the three files. Per §14.7.4 this
   happens at the phase 1 exit.

Bug reports then go to the contacts in `project.yaml`. The 90-day disclosure deadline is tracked with the issue
label `oss-fuzz`.

## Crash triage procedure (§14.7.5)

1. **Reproduce.** `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> <crash-file>` under the same sanitizer. Record the
   stack in the issue.
2. **Minimise.** `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> -minimize_crash=1 -runs=100000 <crash-file>`. Commit the
   minimised input to `core/tests/fuzz/regressions/<target>/<sha1>.bin`.
3. **Classify.**
   - An OVT bug: fix it, keep the regression input, and check the `BitReader` invariant that should have caught
     it (§0.7.1).
   - A bug in a third-party decoder: file it upstream with the minimised input, add a row to
     [`upstream-findings.md`](upstream-findings.md), and mark the test `[xfail-upstream]`.
   - Resource exhaustion by design (for example a 2^32-entry `stsz`): add a limit with a `Diagnostic`
     (code `CNT-00xx`, backbone D2) and document the limit in chapter 8.
4. **Severity.** A memory-safety finding in `core/` is a release blocker. A release branch cannot be tagged with
   an open ASan or UBSan finding.

For a report from OSS-Fuzz, also keep the testcase it provides until the issue is closed, and close the report only
after the fix is on `main` (OSS-Fuzz verifies it itself). Treat the report as private until it is public. The
project's own disclosure policy is in [`SECURITY.md`](../../../SECURITY.md).
