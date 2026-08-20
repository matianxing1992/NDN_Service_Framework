"""Composition of ten policy implementations with explicit default evidence."""

from __future__ import annotations

from dataclasses import asdict, is_dataclass
import hashlib
import json
from typing import Any

from .contracts import POLICY_KINDS


class OptimizationSuite:
    def __init__(self, policies: dict[str, Any], *, name: str, version: str,
                 state_digest: str, placement_strategy=None) -> None:
        unknown = set(policies) - set(POLICY_KINDS)
        if unknown:
            raise ValueError(f"unknown policy kinds: {sorted(unknown)}")
        if not name or not version or not state_digest.startswith("sha256:"):
            raise ValueError("suite identity is incomplete")
        self._policies = dict(policies)
        self.name = name
        self.version = version
        self.state_digest = state_digest
        self._placement_strategy = placement_strategy

    @classmethod
    def defaults(cls) -> "OptimizationSuiteBuilder":
        from ..planner.defaults import DefaultOptimizationSuite
        return OptimizationSuiteBuilder(DefaultOptimizationSuite())

    def policy_names(self) -> tuple[str, ...]:
        return tuple(kind for kind in POLICY_KINDS if kind in self._policies)

    def joint_placement_strategy(self):
        """Return the V2 authority; ten-policy suites are compatibility only."""
        if self._placement_strategy is None:
            raise KeyError("suite has no joint placement strategy")
        return self._placement_strategy

    def policy(self, kind: str) -> Any:
        if kind not in POLICY_KINDS:
            raise ValueError("unknown policy kind")
        try:
            return self._policies[kind]
        except KeyError as exc:
            raise KeyError(f"suite omits policy: {kind}") from exc

    def resolve(self, defaults: "OptimizationSuite") -> tuple["OptimizationSuite", dict[str, bool]]:
        merged: dict[str, Any] = {}
        used_default: dict[str, bool] = {}
        for kind in POLICY_KINDS:
            if kind in self._policies:
                merged[kind] = self._policies[kind]
                used_default[kind] = False
            else:
                merged[kind] = defaults.policy(kind)
                used_default[kind] = True
        return OptimizationSuite(
            merged, name=self.name, version=self.version,
            state_digest=self.state_digest,
            placement_strategy=self._placement_strategy), used_default

    def descriptor(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "version": self.version,
            "policies": [
                _policy_descriptor(kind, self.policy(kind))
                for kind in self.policy_names()
            ],
        }


def _public_configuration(policy: Any) -> dict[str, Any]:
    if is_dataclass(policy):
        value = asdict(policy)
    else:
        value = {
            key: item for key, item in vars(policy).items()
            if not key.startswith("_") and
            isinstance(item, (str, int, float, bool, type(None), list, tuple, dict))
        }
    return value


def _policy_descriptor(kind: str, policy: Any) -> dict[str, Any]:
    return {
        "kind": kind,
        "implementation": type(policy).__module__ + "." + type(policy).__qualname__,
        "version": str(getattr(policy, "version", "1")),
        "configuration": _public_configuration(policy),
    }


class OptimizationSuiteBuilder:
    def __init__(self, defaults: OptimizationSuite):
        self._defaults = defaults
        self._replacements: dict[str, Any] = {}

    def replace(self, **policies) -> "OptimizationSuiteBuilder":
        unknown = set(policies) - set(POLICY_KINDS)
        if unknown:
            raise ValueError(f"unknown policy kinds: {sorted(unknown)}")
        for kind, policy in policies.items():
            if policy is None:
                raise TypeError(f"{kind} policy cannot be None")
            self._replacements[kind] = policy
        return self

    def build(self, *, name: str, version: str) -> OptimizationSuite:
        if not name or not version:
            raise ValueError("suite name and version are required")
        policies = {
            kind: self._replacements.get(kind, self._defaults.policy(kind))
            for kind in POLICY_KINDS
        }
        descriptor = {
            "name": name,
            "version": version,
            "policies": [_policy_descriptor(kind, policies[kind])
                         for kind in POLICY_KINDS],
        }
        encoded = json.dumps(
            descriptor, sort_keys=True, separators=(",", ":")).encode()
        digest = "sha256:" + hashlib.sha256(encoded).hexdigest()
        return OptimizationSuite(
            policies, name=name, version=version, state_digest=digest)


__all__ = ["OptimizationSuite", "OptimizationSuiteBuilder"]
