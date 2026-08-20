"""Opt-in, allowlisted local optimizer and placement-strategy discovery.

Entry points are operator-trusted local code.  The allowlist prevents
accidental package substitution; it is not a process sandbox.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from importlib import metadata
from typing import Any, Iterable, Mapping

from .placement import ModelPlacementStrategy


def distribution_record_digest(distribution: Any) -> str:
    records = sorted(
        "|".join((
            str(item),
            str(getattr(item, "hash", "") or ""),
            str(getattr(item, "size", "") or ""),
        ))
        for item in (distribution.files or ())
    )
    return "sha256:" + hashlib.sha256("\n".join(records).encode()).hexdigest()


def discover_optimizers(allowlist: Iterable[tuple[str, str, str]]) -> dict[str, Any]:
    allowed = {(name, version, digest) for name, version, digest in allowlist}
    discovered: dict[str, Any] = {}
    entry_points = metadata.entry_points()
    candidates = (entry_points.select(group="ndnsf_di.optimizers")
                  if hasattr(entry_points, "select")
                  else entry_points.get("ndnsf_di.optimizers", ()))
    for entry in candidates:
        distribution = entry.dist
        identity = (distribution.metadata["Name"], distribution.version,
                    distribution_record_digest(distribution))
        if identity not in allowed:
            continue
        discovered[entry.name] = entry.load()
    return discovered


@dataclass(frozen=True)
class PlacementStrategyAllowlistEntry:
    distribution: str
    distribution_version: str
    distribution_digest: str
    entry_point: str
    strategy_name: str
    strategy_version: str
    state_digest: str

    def __post_init__(self) -> None:
        values = (
            self.distribution, self.distribution_version, self.entry_point,
            self.strategy_name, self.strategy_version,
        )
        if not all(values):
            raise ValueError("placement strategy allowlist identity is incomplete")
        for name, digest in (
                ("distribution_digest", self.distribution_digest),
                ("state_digest", self.state_digest)):
            if (not digest.startswith("sha256:") or len(digest) != 71):
                raise ValueError(f"{name} must be a canonical sha256 digest")
            try:
                int(digest[7:], 16)
            except ValueError as exc:
                raise ValueError(
                    f"{name} must be a canonical sha256 digest") from exc


def discover_placement_strategies(
    allowlist: Iterable[PlacementStrategyAllowlistEntry],
) -> dict[str, ModelPlacementStrategy]:
    """Load only exact package, entry-point, API, and strategy identities."""

    allowed = {entry.entry_point: entry for entry in allowlist}
    discovered: dict[str, ModelPlacementStrategy] = {}
    entry_points = metadata.entry_points()
    candidates = (
        entry_points.select(group="ndnsf_di.placement_strategies")
        if hasattr(entry_points, "select")
        else entry_points.get("ndnsf_di.placement_strategies", ())
    )
    for entry in candidates:
        expected = allowed.get(entry.name)
        if expected is None:
            continue
        distribution = entry.dist
        actual_distribution = (
            distribution.metadata["Name"],
            distribution.version,
            distribution_record_digest(distribution),
        )
        expected_distribution = (
            expected.distribution,
            expected.distribution_version,
            expected.distribution_digest,
        )
        if actual_distribution != expected_distribution:
            continue
        loaded = entry.load()
        strategy = (
            loaded()
            if not isinstance(loaded, ModelPlacementStrategy) and
            callable(loaded)
            else loaded
        )
        if not isinstance(strategy, ModelPlacementStrategy):
            raise TypeError(
                f"allowlisted placement entry point {entry.name!r} "
                "did not return ModelPlacementStrategy")
        identity = (
            strategy.name, strategy.version, strategy.state_digest)
        expected_identity = (
            expected.strategy_name,
            expected.strategy_version,
            expected.state_digest,
        )
        if identity != expected_identity:
            raise ValueError(
                f"allowlisted placement strategy identity mismatch: {entry.name}")
        if strategy.name in discovered:
            raise ValueError(
                f"duplicate placement strategy name: {strategy.name}")
        discovered[strategy.name] = strategy
    return discovered


def select_placement_strategy(
    configuration: Mapping[str, Any],
    strategies: Mapping[str, ModelPlacementStrategy],
) -> ModelPlacementStrategy:
    """Select the exact digest-pinned strategy named by ``app.yaml``."""

    try:
        pin = configuration["planning"]["strategy"]
        name = str(pin["name"])
        version = str(pin["version"])
        state_digest = str(pin["digest"])
    except (KeyError, TypeError) as exc:
        raise ValueError("app configuration has no complete strategy pin") from exc
    strategy = strategies.get(name)
    if strategy is None:
        raise ValueError("configured placement strategy is not allowlisted")
    if (strategy.version != version or
            strategy.state_digest != state_digest):
        raise ValueError("configured placement strategy pin mismatch")
    return strategy


__all__ = [
    "PlacementStrategyAllowlistEntry", "discover_optimizers",
    "discover_placement_strategies", "distribution_record_digest",
    "select_placement_strategy",
]
