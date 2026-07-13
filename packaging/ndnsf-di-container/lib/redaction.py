"""Conservative redaction for deployment diagnostics and evidence."""

from __future__ import annotations

import re
from typing import Any


SECRET_KEY_RE = re.compile(r"(?:password|passwd|secret|token|private[_-]?key|credential)", re.IGNORECASE)
SECRET_VALUE_RE = re.compile(
    r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|(?:password|token|secret)\s*[=:]\s*\S+",
    re.IGNORECASE,
)
REDACTED = "<redacted>"


def redact(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: (REDACTED if SECRET_KEY_RE.search(str(key)) else redact(item)) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, str):
        return SECRET_VALUE_RE.sub(REDACTED, value)
    return value


def secret_findings(value: Any) -> list[str]:
    findings: list[str] = []

    def visit(item: Any, path: str) -> None:
        if isinstance(item, dict):
            for key, child in item.items():
                child_path = f"{path}.{key}"
                if SECRET_KEY_RE.search(str(key)) and child != REDACTED:
                    findings.append(child_path)
                visit(child, child_path)
        elif isinstance(item, list):
            for index, child in enumerate(item):
                visit(child, f"{path}[{index}]")
        elif isinstance(item, str) and SECRET_VALUE_RE.search(item):
            findings.append(path)

    visit(value, "$")
    return findings
