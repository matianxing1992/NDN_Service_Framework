"""Instance-scoped optimizer registry; never process-global mutable state."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

from .contracts import POLICY_KINDS


@dataclass(frozen=True)
class RegisteredPolicy:
    kind: str
    name: str
    version: str
    implementation: Any


class PolicyRegistry:
    def __init__(self) -> None:
        self._items: dict[str, RegisteredPolicy] = {}

    def register(self, kind: str, implementation: Any, *, name: str,
                 version: str) -> None:
        if kind not in POLICY_KINDS:
            raise ValueError("unknown policy kind")
        if kind in self._items:
            raise ValueError("duplicate policy registration")
        if not name or not version:
            raise ValueError("policy identity requires name and version")
        self._items[kind] = RegisteredPolicy(kind, name, version, implementation)

    def get(self, kind: str) -> RegisteredPolicy:
        try:
            return self._items[kind]
        except KeyError as exc:
            raise KeyError(f"policy is not registered: {kind}") from exc

    def snapshot(self) -> Mapping[str, RegisteredPolicy]:
        return dict(self._items)


__all__ = ["PolicyRegistry", "RegisteredPolicy"]
