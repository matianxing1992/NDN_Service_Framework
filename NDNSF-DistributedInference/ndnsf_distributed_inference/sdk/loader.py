"""Opt-in, allowlisted optimizer entry-point discovery."""

from __future__ import annotations

import hashlib
from importlib import metadata
from typing import Any, Iterable


def distribution_record_digest(distribution: Any) -> str:
    records = sorted(str(item) for item in (distribution.files or ()))
    return "sha256:" + hashlib.sha256("\n".join(records).encode()).hexdigest()


def discover_optimizers(allowlist: Iterable[tuple[str, str, str]]) -> dict[str, Any]:
    allowed = {(name, version, digest) for name, version, digest in allowlist}
    discovered: dict[str, Any] = {}
    entry_points = metadata.entry_points()
    candidates = (entry_points.select(group="ndnsf_di.optimizers")
                  if hasattr(entry_points, "select")
                  else entry_points.get("ndnsf_di.optimizers", ()))
    for entry in candidates:
        distribution = entry.dist
        identity = (distribution.metadata["Name"], distribution.version,
                    distribution_record_digest(distribution))
        if identity not in allowed:
            continue
        discovered[entry.name] = entry.load()
    return discovered


__all__ = ["discover_optimizers", "distribution_record_digest"]
