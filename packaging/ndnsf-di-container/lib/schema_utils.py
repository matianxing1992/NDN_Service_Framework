"""Shared JSON Schema helpers for the Spec 108 deployment package."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator, FormatChecker


LIB_ROOT = Path(__file__).resolve().parent
PACKAGE_ROOT = LIB_ROOT.parent
SCHEMA_ROOT = PACKAGE_ROOT / "schemas"


class SchemaValidationError(ValueError):
    """A value does not satisfy a checked-in deployment contract."""


def load_schema(name: str) -> dict[str, Any]:
    path = SCHEMA_ROOT / name
    return json.loads(path.read_text(encoding="utf-8"))


def validate_schema(value: Any, schema_name: str) -> None:
    validator = Draft202012Validator(load_schema(schema_name), format_checker=FormatChecker())
    errors = sorted(validator.iter_errors(value), key=lambda item: list(item.absolute_path))
    if errors:
        error = errors[0]
        location = ".".join(str(part) for part in error.absolute_path) or "$"
        raise SchemaValidationError(f"{location}: {error.message}")
