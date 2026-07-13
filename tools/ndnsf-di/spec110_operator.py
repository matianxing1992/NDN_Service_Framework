#!/usr/bin/env python3
"""Shared profile and command safety checks for the Spec 110 operator CLI."""

from __future__ import annotations

from pathlib import Path
import json
import re
from typing import Mapping


FORBIDDEN_KEYS = re.compile(
    r"(?:password|passphrase|private.?key|mfa|credential|bearer|access.?token|registry.?token|ssh.?key|secret)",
    re.IGNORECASE,
)
SHELL_FIELDS = {
    "account", "qos", "partition", "gres", "jobName", "comment", "command",
    "executable", "argument", "arguments", "node", "address", "transport",
}
SHELL_META = re.compile(r"[\x00\r\n;&|`$<>]")
ALLOWED_TOKEN_FIELDS = {
    "tokenizerDigest", "tokenizerRevision", "inputTokenIds", "outputTokenIds",
    "oracleTokenIds", "userTokenVerified", "providerTokenVerified",
}


class OperatorSafetyError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise OperatorSafetyError(code + (f":{detail}" if detail else ""))


def validate_safe_document(value: object, path: str = "$") -> None:
    if isinstance(value, Mapping):
        for key, item in value.items():
            name = str(key)
            if name not in ALLOWED_TOKEN_FIELDS and FORBIDDEN_KEYS.search(name):
                _fail("OPERATOR_CREDENTIAL_FIELD_FORBIDDEN", path + "." + name)
            if name in SHELL_FIELDS:
                values = item if isinstance(item, list) else [item]
                for candidate in values:
                    if isinstance(candidate, str) and SHELL_META.search(candidate):
                        _fail("OPERATOR_COMMAND_INJECTION", path + "." + name)
            validate_safe_document(item, path + "." + name)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            validate_safe_document(item, f"{path}[{index}]")
    elif isinstance(value, str) and ("\x00" in value or "\r" in value or "\n" in value):
        _fail("OPERATOR_MULTILINE_VALUE_FORBIDDEN", path)


def load_safe_json(path: Path | str):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        _fail("OPERATOR_JSON_INVALID", str(error))
    validate_safe_document(value)
    return value


__all__ = ["OperatorSafetyError", "load_safe_json", "validate_safe_document"]
