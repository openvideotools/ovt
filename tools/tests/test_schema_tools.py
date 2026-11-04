# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_schema_in_spec.py and tools/schema_hash.py (WI-0.080)."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import check_schema_in_spec  # noqa: E402
import schema_hash  # noqa: E402

SPEC = """# 6 Data model

## 6.3 `core/schema/ovt_model.fbs`

```fbs
// core/schema/ovt_model.fbs — test schema
namespace ovt.fb;
table A { x:int; }
root_type A;
```

Some prose with another block that is not a schema:

```fbs
table NotASchema {}
```

## 6.4 `core/schema/ovt_container.fbs`

```fbs
// core/schema/ovt_container.fbs — second schema
namespace ovt.fb;
table B { y:int; }
```
"""


def test_extract_block_finds_the_block_by_basename() -> None:
    block = check_schema_in_spec.extract_block(SPEC, "ovt_container.fbs")
    assert block == "// core/schema/ovt_container.fbs — second schema\nnamespace ovt.fb;\ntable B { y:int; }\n"
    with pytest.raises(LookupError):
        check_schema_in_spec.extract_block(SPEC, "ovt_metrics.fbs")


def test_write_then_check_round_trips(tmp_path: Path) -> None:
    spec = tmp_path / "06.md"
    spec.write_text(SPEC, encoding="utf-8")
    schema = tmp_path / "ovt_model.fbs"
    assert check_schema_in_spec.main(["--write", str(spec), str(schema)]) == 0
    assert schema.read_text(encoding="utf-8").startswith("// core/schema/ovt_model.fbs")
    assert check_schema_in_spec.main([str(spec), str(schema)]) == 0
    # CRLF checkouts still match, and so does a file whose trailing whitespace a hook stripped.
    schema.write_bytes(schema.read_bytes().replace(b"\n", b"\r\n"))
    assert check_schema_in_spec.main([str(spec), str(schema)]) == 0
    spec.write_text(SPEC.replace("table A { x:int; }", "table A { x:int; }   "), encoding="utf-8")
    assert check_schema_in_spec.main([str(spec), str(schema)]) == 0


def test_check_reports_a_difference(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    spec = tmp_path / "06.md"
    spec.write_text(SPEC, encoding="utf-8")
    schema = tmp_path / "ovt_model.fbs"
    schema.write_text(
        "// core/schema/ovt_model.fbs — test schema\nnamespace ovt.fb;\ntable A { x:long; }\nroot_type A;\n"
    )
    assert check_schema_in_spec.main([str(spec), str(schema)]) == 1
    out = capsys.readouterr()
    assert "-table A { x:int; }" in out.out and "+table A { x:long; }" in out.out
    assert check_schema_in_spec.main([str(spec), str(tmp_path / "ovt_metrics.fbs")]) == 2


def test_repository_schemas_match_the_spec() -> None:
    spec = REPO / "docs" / "spec" / "06-data-model.md"
    schemas = sorted((REPO / "core" / "schema").glob("*.fbs"))
    assert schemas, "no schema under core/schema"
    assert check_schema_in_spec.main([str(spec), *map(str, schemas)]) == 0


def test_schema_hash_is_sha256_of_lf_text(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    lf = tmp_path / "a.fbs"
    lf.write_bytes(b"table A {}\n")
    crlf = tmp_path / "b.fbs"
    crlf.write_bytes(b"table A {}\r\n")
    expected = hashlib.sha256(b"table A {}\n").hexdigest()
    assert schema_hash.schema_hash(lf) == expected
    assert schema_hash.schema_hash(crlf) == expected
    assert schema_hash.main([str(lf)]) == 0
    assert capsys.readouterr().out == f"{expected}  {lf}\n"
    assert schema_hash.main([str(tmp_path / "missing.fbs")]) == 2
    assert schema_hash.main([]) == 2
