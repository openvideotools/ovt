# WI-0.601 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, Python 3.13, pre-commit 4.0.1. Date: 2026-11-04. Spec
version: 1.0.0-rc1. Commit: the head of the WI-0.601 pull request.

Automated tests: `tools/tests/test_spdx_tools.py` (19 tests).

## Step 4: headers on all files

All tracked source files already carried the correct header, written by WI-0.000 and the later M0.1 PRs.
The inserter therefore changed nothing, and this PR has no `licence:` header commit. The checker also passes
on the heads of the other stacked branches (#14 WI-0.008, #16 WI-0.010, #19 WI-0.006):

```console
$ python3 -I tools/check_spdx_dirs.py; echo $?        # on this branch and on each of the three heads
0
```

## Test 1: `tools/check_spdx_dirs.py` on the repository exits 0

```console
$ python3 -I tools/check_spdx_dirs.py; echo $?
0
```

Automated: `test_repository_passes`. Result: pass.

## Test 2: a GPL header in `core/src/x.cpp` fails pre-commit with `E-HEADER`

<!-- REUSE-IgnoreStart -->

```console
$ printf '// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors\n// SPDX-License-Identifier: GPL-3.0-or-later\nint x;\n' > core/src/x.cpp
$ git add core/src/x.cpp && pre-commit run --files core/src/x.cpp; echo "exit $?"
...
SPDX header present (tools/spdx_header.py)..........................................Failed
- hook id: spdx-header
- exit code: 1

E-HEADER core/src/x.cpp expected LGPL-2.1-or-later got GPL-3.0-or-later

SPDX licence matches the directory (tools/check_spdx_dirs.py).......................Failed
- hook id: check-spdx-dirs
- exit code: 1

E-HEADER core/src/x.cpp expected LGPL-2.1-or-later got GPL-3.0-or-later

exit 1
```

(The file was removed afterwards.) Automated: `test_gpl_header_in_core_fails`. Result: pass.

## Test 3: shebang Python file: header on line 2, a second run changes nothing

```console
$ printf '#!/usr/bin/env python3\nprint("hi")\n' > tools/zz/t.py
$ python3 -I tools/spdx_header.py tools/zz/t.py; echo "exit $?"
inserted LGPL-2.1-or-later header into tools/zz/t.py
exit 1
$ head -3 tools/zz/t.py
#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
$ python3 -I tools/spdx_header.py tools/zz/t.py; echo "exit $?"
exit 0
$ python3 -I tools/spdx_header.py --check tools/zz/t.py; echo "exit $?"
exit 0
```

Automated: `test_shebang_python_gets_the_header_on_line_2`. Result: pass.

## REUSE

The two tools quote SPDX header lines in their code, so the code after their own header sits between
`REUSE-IgnoreStart`/`REUSE-IgnoreEnd`. `reuse lint` on this branch: "Congratulations! Your project is
compliant with version 3.3 of the REUSE Specification :-)".

<!-- REUSE-IgnoreEnd -->
