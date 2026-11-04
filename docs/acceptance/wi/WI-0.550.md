# WI-0.550 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, Ninja 1.11.1,
Catch2 3.4.0 (system package), Python 3.13 with pytest 8.3.4.
Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.550 pull request.

Configured with `-DOVT_USE_VCPKG=OFF` (system packages) because the session's network refuses vcpkg's
source downloads (see `docs/acceptance/wi/WI-0.002.md`); CI runs the same tests through vcpkg from WI-0.030
on. Python: a venv with `pip install -r tools/requirements-test.txt`, passed as `-DPython3_EXECUTABLE`.

## Test 1: `ctest --preset pr` exits 0 and the JUnit file lists ≥ 1 test

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DPython3_EXECUTABLE=/tmp/venv/bin/python
$ cmake --build --preset linux-x64-debug
$ ctest --preset pr
...
8/8 Test #8: tools_pytest .................................................   Passed    0.32 sec

100% tests passed, 0 tests failed out of 8

Label Time Summary:
fast            =   0.36 sec*proc (8 tests)
golden          =   0.00 sec*proc (1 test)
test_support    =   0.04 sec*proc (6 tests)
tools           =   0.32 sec*proc (1 test)
unit            =   0.04 sec*proc (7 tests)
version         =   0.00 sec*proc (1 test)
$ echo $?
0
$ grep -c "<testcase" build/pr/junit.xml
8
```

The JUnit file of a test preset is `build/<test-preset>/junit.xml`, so the `pr` lane writes
`build/pr/junit.xml` (`outputJUnitFile` uses `${presetName}`).

Result: pass.

## Test 2: `OVT_REQUIRE_CORPORA=1` fails a missing corpus, otherwise it is skipped

The `[needs-corpus]` demonstration test is hidden (`[.]`), so the suite never depends on a deliberately
absent corpus. It is run explicitly:

```console
$ build/linux-x64-debug/core/tests/ovt_core_tests "[needs-corpus-demo]"; echo rc=$?
...
/ovt/core/tests/test_support/corpus.cpp:60: SKIPPED:
explicitly with message:
  corpus file missing: syn_wi0550_absent

test cases: 1 | 1 skipped
rc=4
$ OVT_REQUIRE_CORPORA=1 build/linux-x64-debug/core/tests/ovt_core_tests "[needs-corpus-demo]"; echo rc=$?
ovt_core_tests: OVT_REQUIRE_CORPORA=1, missing corpus files fail (corpora: /home/claude/ovt/build/corpora)
...
/ovt/core/tests/test_support/corpus.cpp:58: FAILED:
explicitly with message:
  corpus file missing: syn_wi0550_absent

test cases: 1 | 1 failed
rc=1
```

Exit code 4 is Catch2's "all selected tests skipped"; every discovered test has `SKIP_RETURN_CODE 4`, so
ctest reports such a test as skipped, not failed. The automated test
`test_support/corpus: finds verified entries only` covers the lookup itself (`.ok` marker, layout).

Result: pass.

## Test 3: `OVT_UPDATE_GOLDEN=1` rewrites the golden file and the test passes

```console
$ cp tests/golden/test_support/normalize_json.json /tmp/g.json
$ sed -i 's/39.500000/0.0/' tests/golden/test_support/normalize_json.json      # corrupt the golden
$ ctest --preset linux-x64-debug -R "matches its golden"
1/1 Test #6: test_support/json: normalize_json matches its golden file ...***Failed    0.00 sec
$ OVT_UPDATE_GOLDEN=1 ctest --preset linux-x64-debug -R "matches its golden"
1/1 Test #6: test_support/json: normalize_json matches its golden file ...   Passed    0.00 sec
$ cmp tests/golden/test_support/normalize_json.json /tmp/g.json && echo "rewritten to original"
rewritten to original
$ ctest --preset linux-x64-debug -R "matches its golden"
1/1 Test #6: test_support/json: normalize_json matches its golden file ...   Passed    0.00 sec
```

The automated test `test_support/golden: compare, mismatch and update` covers the missing / updated /
matched / mismatch outcomes.

Result: pass.

## Test 4: only `ovt_core_tests` defines its own `main`

Every linked executable has a `main` symbol (Catch2WithMain supplies one), so the check looks at which of
the target's own object files defines it and which Catch2 library it links:

```console
$ for t in ovt_core_tests ovt_tests_version; do ...; done
ovt_core_tests: objects defining main: main.cpp.o | links: libCatch2.a
ovt_tests_version: objects defining main: | links: libCatch2.a libCatch2Main.a
$ nm -C build/linux-x64-debug/core/tests/CMakeFiles/ovt_core_tests.dir/main.cpp.o | grep " T main"
0000000000000000 T main
```

Result: pass.

## Other checks

- `-DOVT_WERROR=ON`: 0 warnings with GCC 13 and Clang 18; all 7 C++ tests pass with Clang too.
- `linux-x64-asan-ubsan` (GCC, see WI-0.004's record): `ctest --preset sanitizers` passes 7/7.
- `pytest tools/tests`: 17 passed (the `tools/ctest.sh` mapping of WI-0.001, now automated).
