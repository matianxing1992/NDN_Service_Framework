"""Read-only Spec 111 execution-certificate import shim.

Removal gate: delete after a later migration Spec observes zero legacy journal
imports and zero external imports for one supported release cycle. This module
must never participate in execution authority or write legacy state.
"""

from __future__ import annotations

import warnings

from ..core.contracts import ExecutionCommitCertificate as _LegacyCertificate


def read_execution_commit_certificate(wire: bytes) -> _LegacyCertificate:
    warnings.warn(
        "ExecutionCommitCertificate is deprecated; import records only and "
        "authorize with ExecutionActivateMessage",
        DeprecationWarning,
        stacklevel=2,
    )
    return _LegacyCertificate.from_bytes(wire)


__all__ = ["read_execution_commit_certificate"]
