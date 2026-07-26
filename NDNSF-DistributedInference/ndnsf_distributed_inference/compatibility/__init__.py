"""Thin legacy export adapters for NDNSF-DI migration releases."""

from .exports import legacy_names, resolve_legacy_export
from .adapters import (
    LegacyClientAdapter, LegacyProviderAdapter,
    LegacyProviderLifecycleAdapter,
)

__all__ = [
    "LegacyClientAdapter", "LegacyProviderAdapter",
    "LegacyProviderLifecycleAdapter", "legacy_names",
    "resolve_legacy_export",
]
