# WI-0.557 acceptance record: acceptance test 2, local run

Machine: cloud container, Ubuntu 24.04 x86_64, Clang 18.1.3 (libFuzzer), CMake 3.28.3, Ninja.
Date: 2026-11-06. Spec version: 1.0-rc2. Commit: the head of the WI-0.557 pull request (#91).

**What this proves, and what it does not.** GitHub-hosted runners are off (`OVT_HOSTED_RUNNERS=off`,
billing), so acceptance test 2 has no CI run. This record runs the exact `Fuzz 60 s per target` step script
of `.github/workflows/fuzz-smoke.yml` (extracted from the YAML, shard 0, under `bash --noprofile --norc -eo
pipefail`, the shell `shell: bash` gives) against a build configured like the `linux-x64-fuzz` preset
(`OVT_ENABLE_FUZZ=ON`, empty `OVT_SANITIZER`, so the auto choice `fuzz`). It proves that a crash makes the
step fail (the `| tee` does not hide the exit status) and that `crash-*` lands in `fuzz-artifacts/`, and that
the coverage guard passes with instrumentation. It does **not** prove the `Upload reproducers` step, the
corpus cache, or clang-16 from apt.llvm.org. That part of AT2 is owed once hosted runners return (tracked in
the issue linked from the PR). Karl's explicit acceptance of this record is needed for the pull request
(chapter 14 §14.1.2 item 6).

The container has no vcpkg access, so the build used system packages and clang 18:

```console
$ cmake --preset linux-x64-fuzz -B /tmp/b-fz -DOVT_USE_VCPKG=OFF -DOVT_SYSTEM_DAV1D=ON \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_LAUNCHER=
$ ln -s /tmp/b-fz build/linux-x64-fuzz
```

## Planted bug: step fails, artefact written

`BitReader::bytes()` in `core/src/bitreader/bitreader.cpp` returned one byte too many:
`return data_.subspan(byte_off, n + 1);`

```console
$ PRESET=linux-x64-fuzz SHARD=0 bash --noprofile --norc -eo pipefail fuzz-step.sh
...
INFO: Loaded 1 modules   (12718 inline 8-bit counters)
==1877== ERROR: libFuzzer: deadly signal
SUMMARY: libFuzzer: deadly signal
artifact_prefix='fuzz-artifacts/'; Test unit written to fuzz-artifacts/crash-d983e2a1eaeffe4c310e3a6c07873bb91d334f7a
$ echo $?
77
$ ls fuzz-artifacts
bitreader.log
crash-d983e2a1eaeffe4c310e3a6c07873bb91d334f7a
```

The step exits 77 (libFuzzer's crash code) within the 60 s budget, and the reproducer is in `fuzz-artifacts/`,
the directory `Upload reproducers` uploads. The fuzz target's own invariant (`bytes()` size) caught the bug;
the failure is the target's `abort()`, reported as a deadly signal.

## Same build, bug reverted: step passes with coverage

```console
$ git checkout core/src/bitreader/bitreader.cpp   # rebuild, rerun the same script
$ echo $?
0
#9 INITED cov: 696 ft: 1696 corp: 8/1283b exec/s: 0 rss: 36Mb
#31828 DONE   cov: 796 ft: 5661 corp: 429/44Kb lim: 192 exec/s: 521 rss: 464Mb
stat::new_units_added:          986
```

`INITED cov: 696` (N > 0) and `new_units_added` > 0 show libFuzzer has coverage feedback with the default
`OVT_SANITIZER`, and the coverage guard in the step did not fire. The planted change is not committed.
