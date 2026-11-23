"""Canonical native Provider API for NDNSF-DI.

The provider facade is a direct view of the C++ owner.  This module is a
stable import path; it does not duplicate Provider admission, assembly,
runner, or shutdown state in Python.
"""

from .api import (
    ProviderConfig,
    ServiceDefinition,
    ProviderCounters,
    ProviderRegistration,
    Provider,
)

__all__ = [
    "ProviderConfig",
    "ServiceDefinition",
    "ProviderCounters",
    "ProviderRegistration",
    "Provider",
]
