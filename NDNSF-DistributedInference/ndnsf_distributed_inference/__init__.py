"""NDNSF distributed-inference package.

Root-level names are retained as lazy compatibility exports.  Importing a
sub-package such as :mod:`ndnsf_distributed_inference.core` therefore does not
eagerly load APP, planner, model adapter, GUI or operations owners.
"""

from __future__ import annotations

from typing import Any

from .compatibility.exports import legacy_names, resolve_legacy_export

__all__ = list(legacy_names())


def __getattr__(name: str) -> Any:
    if name not in __all__:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    value = resolve_legacy_export(name)
    globals()[name] = value
    return value


def __dir__() -> list[str]:
    return sorted(set(globals()) | set(__all__))
