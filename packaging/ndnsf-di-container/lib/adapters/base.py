"""Thin execution-adapter contract for Spec 108."""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any


REQUIRED_OPERATIONS = ("preflight", "materialize", "start", "status", "logs", "evidence", "stop")


class Adapter(ABC):
    @abstractmethod
    def preflight(self, profile: dict[str, Any]) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def materialize(self, profile: dict[str, Any]) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def start(self, profile: dict[str, Any]) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def status(self, reference: str) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def logs(self, reference: str) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def evidence(self, reference: str) -> dict[str, Any]: raise NotImplementedError
    @abstractmethod
    def stop(self, reference: str) -> dict[str, Any]: raise NotImplementedError


def adapter_missing_operations(adapter_type: type[Any]) -> list[str]:
    missing = []
    for name in REQUIRED_OPERATIONS:
        implementation = getattr(adapter_type, name, None)
        base = getattr(Adapter, name, None)
        if implementation is None or implementation is base or getattr(implementation, "__isabstractmethod__", False):
            missing.append(name)
    return missing
