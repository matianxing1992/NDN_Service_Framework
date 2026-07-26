"""Independent instance-scoped RunnerAdapter registry."""

from __future__ import annotations

from typing import Any, Iterable, Mapping


class RunnerAdapterRegistry:
    def __init__(self) -> None:
        self._adapters: dict[str, Any] = {}

    def register(self, adapter: Any) -> None:
        name = str(getattr(adapter, "name", ""))
        version = str(getattr(adapter, "version", ""))
        if not name or not version:
            raise ValueError("Runner adapter requires name and version")
        if name in self._adapters:
            raise ValueError("duplicate Runner adapter")
        if not callable(getattr(adapter, "supports", None)) or not callable(
                getattr(adapter, "create_runner", None)):
            raise TypeError("invalid Runner adapter")
        self._adapters[name] = adapter

    def resolve(self, name: str, target: Any) -> Any:
        if name not in self._adapters:
            raise KeyError(f"Runner adapter is not registered: {name}")
        adapter = self._adapters[name]
        if not adapter.supports(target):
            raise ValueError("Runner adapter does not support selected target")
        return adapter

    def create(self, name: str, target: Any, artifacts: Iterable[str]) -> Any:
        return self.resolve(name, target).create_runner(target, artifacts)

    def snapshot(self) -> Mapping[str, Any]:
        return dict(self._adapters)


__all__ = ["RunnerAdapterRegistry"]
