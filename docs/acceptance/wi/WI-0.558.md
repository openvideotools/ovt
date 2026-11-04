# WI-0.558 acceptance record

Machine: Linux 6.18 x86_64 cloud container, GCC 13.3.0. Date: 2026-11-04.

| # | Given / When / Then | Evidence |
|---|---|---|
| 1 | asan preset, `ctest --preset sanitizers`, exit 0 | **Not run in this session** (no flatc in the container, see the PR body). The suppression files are empty, so they change no sanitizer outcome. Run by the owner or the WI-0.031 jobs. |
| 2 | A suppression line without a URL, `check_suppressions.py`, exit 1 | Automated: `tools/tests/test_check_suppressions.py::test_entry_without_url_fails` |
| 3 | A deliberate data race in a scratch test, under tsan, the job fails | Manual, below. |
| – | Third-party builds receive the sanitizer flags | Automated: `tools/tests/test_third_party_tools.py::test_third_party_builds_get_the_asan_flags`, `..._tsan_flags`, `..._no_flags_without_sanitizers` |

## Test 3: manual record

Scratch program (not committed): two threads each run `++g` 100 000 times on a global `int g`.

```sh
g++ -fsanitize=thread -O1 -g race.cpp -o race -pthread
TSAN_OPTIONS=halt_on_error=1 ./race; echo "exit status $?"
```

Output (first lines, then the last line):

```text
==================
WARNING: ThreadSanitizer: data race (pid=1889)
  Read of size 4 at 0x55f3ea3b201c by thread T2:
...
exit status 66
```
