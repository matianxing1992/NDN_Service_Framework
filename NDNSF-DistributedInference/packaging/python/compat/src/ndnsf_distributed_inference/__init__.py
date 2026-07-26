"""Lazy root compatibility exports for the aggregate distribution."""

from typing import Any
from .compatibility.exports import legacy_names, resolve_legacy_export

__all__ = list(legacy_names())

def __getattr__(name: str) -> Any:
    if name not in __all__:
        raise AttributeError(name)
    value = resolve_legacy_export(name)
    globals()[name] = value
    return value

def __dir__(): return sorted(set(globals()) | set(__all__))
