"""Spec 165 compatibility import for generic NDNSF deadline semantics."""

from ndnsf.progress_deadline import (  # noqa: F401
    DeadlineMonitor,
    DeadlineTerminal,
    ProgressDecision,
    ProgressObservation,
)

__all__ = [
    "DeadlineMonitor",
    "DeadlineTerminal",
    "ProgressDecision",
    "ProgressObservation",
]
