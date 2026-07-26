"""Lazy adapter for the existing native plan/session/runner seam."""

from __future__ import annotations

from typing import Any, Callable

from .contracts import CoreAssignment, CoreExecutionPlan


class NativeBindingAdapter:
    """Binds a caller-supplied native session factory without importing models."""

    def __init__(self, factory: Callable[[CoreExecutionPlan, CoreAssignment], Any]) -> None:
        if not callable(factory):
            raise TypeError("native session factory must be callable")
        self._factory = factory

    def create_session(self, plan: CoreExecutionPlan, assignment: CoreAssignment) -> Any:
        session = self._factory(plan, assignment)
        if not callable(getattr(session, "execute", None)):
            raise TypeError("native session must expose execute(payload)")
        if not callable(getattr(session, "cancel", None)):
            raise TypeError("native session must expose cancel()")
        return session
