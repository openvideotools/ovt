# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""docs/spec/schemas/settings.schema.json (chapter 11 §11.2.5, WI-1.450) is a valid JSON Schema 2020-12.

The GUI validates settings.json with a small subset validator of its own (app/src/settings.cpp); this test
holds the same schema to the reference validator, so the subset cannot drift from the real semantics.
"""

from __future__ import annotations

import json
from pathlib import Path

import jsonschema
import pytest

SCHEMA_PATH = Path(__file__).resolve().parents[2] / "docs" / "spec" / "schemas" / "settings.schema.json"


@pytest.fixture(name="validator")
def _validator() -> jsonschema.Draft202012Validator:
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def test_defaults_validate(validator: jsonschema.Draft202012Validator) -> None:
    validator.validate(
        {
            "settings_version": 1,
            "general": {"theme": "system", "recent_files_count": 10},
        }
    )


def test_unknown_keys_are_allowed(validator: jsonschema.Draft202012Validator) -> None:
    validator.validate({"settings_version": 1, "future": {"x": 1}, "general": {"extra": True}})


@pytest.mark.parametrize(
    "document",
    [
        {},
        {"settings_version": 2},
        {"settings_version": 1, "general": {"theme": "purple"}},
        {"settings_version": 1, "general": {"recent_files_count": 1000}},
        {"settings_version": 1, "general": {"confirm_on_close": "yes"}},
    ],
)
def test_invalid_documents_are_rejected(validator: jsonschema.Draft202012Validator, document: dict) -> None:
    with pytest.raises(jsonschema.ValidationError):
        validator.validate(document)
