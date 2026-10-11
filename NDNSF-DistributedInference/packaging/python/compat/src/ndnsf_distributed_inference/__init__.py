"""Lazy root compatibility exports for the aggregate distribution."""

from typing import Any
from pkgutil import extend_path

# Distribution wheels share this package across installation locations.
__path__ = extend_path(__path__, __name__)

from .compatibility.exports import legacy_names, resolve_legacy_export

__all__ = list(legacy_names())

def __getattr__(name: str) -> Any:
    if name not in __all__:
        raise AttributeError(name)
    value = resolve_legacy_export(name)
    globals()[name] = value
    return value

def __dir__(): return sorted(set(globals()) | set(__all__))
