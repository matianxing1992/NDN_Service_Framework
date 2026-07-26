"""Atomic execution-intent prepare/revalidate/commit/abort/release state."""

from __future__ import annotations

from dataclasses import replace
from threading import Lock
from typing import Callable

from .ports import ValidatedExecutionIntent


class ExecutionIntentCoordinator:
    def __init__(self) -> None:
        self._intents: dict[str, ValidatedExecutionIntent] = {}
        self._lock = Lock()

    def prepare(self, intent: ValidatedExecutionIntent) -> ValidatedExecutionIntent:
        with self._lock:
            current = self._intents.get(intent.intent_id)
            if current is not None:
                if current == intent:
                    return current
                raise ValueError("conflicting execution intent")
            if intent.state != "PREPARED":
                raise ValueError("new execution intent must be PREPARED")
            self._intents[intent.intent_id] = intent
            return intent

    def revalidate(self, intent_id: str, validator: Callable[[ValidatedExecutionIntent], None]) -> None:
        with self._lock:
            intent = self._intents[intent_id]
        validator(intent)

    def commit(self, intent_id: str) -> ValidatedExecutionIntent:
        with self._lock:
            intent = self._intents[intent_id]
            if intent.state == "COMMITTED":
                return intent
            if intent.state != "PREPARED":
                raise ValueError("only prepared intent may commit")
            committed = replace(intent, state="COMMITTED")
            self._intents[intent_id] = committed
            return committed

    def abort(self, intent_id: str) -> ValidatedExecutionIntent:
        with self._lock:
            intent = self._intents[intent_id]
            if intent.state == "COMMITTED":
                raise ValueError("committed intent cannot be aborted")
            aborted = replace(intent, state="ABORTED")
            self._intents[intent_id] = aborted
            return aborted

    def release(self, intent_id: str) -> ValidatedExecutionIntent:
        with self._lock:
            intent = self._intents[intent_id]
            released = replace(intent, state="RELEASED")
            self._intents[intent_id] = released
            return released

    def get(self, intent_id: str) -> ValidatedExecutionIntent:
        with self._lock:
            return self._intents[intent_id]


__all__ = ["ExecutionIntentCoordinator"]
