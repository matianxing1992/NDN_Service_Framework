"""Non-authoritative migration adapter for the pre-Spec-163 policy graph."""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping


@dataclass(frozen=True)
class LegacyPlacementHints:
    """Old partition/provider outputs retained only as advisory evidence."""

    results: Mapping[str, Any]
    authoritative: bool = False
    source: str = "legacy-ten-policy-compatibility"

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "results", MappingProxyType(dict(self.results)))
        if self.authoritative:
            raise ValueError("legacy placement hints cannot be authoritative")

    def to_placement_decision(self):
        raise RuntimeError(
            "legacy partition/provider hints cannot authorize a V2 placement")


class LegacyPlacementCompatibilityAdapter:
    """Run old APPClient.decide requests without granting plan authority."""

    _ALLOWED_KINDS = ("partition", "provider_assignment")

    def __init__(self, app_client: Any) -> None:
        self._app_client = app_client

    def collect(self, requests: Mapping[str, Any]) -> LegacyPlacementHints:
        selected = {
            kind: requests[kind]
            for kind in self._ALLOWED_KINDS
            if kind in requests
        }
        if not selected:
            return LegacyPlacementHints({})
        outputs = self._app_client.decide(selected)
        return LegacyPlacementHints({
            kind: outputs[kind]
            for kind in self._ALLOWED_KINDS
            if kind in outputs
        })


__all__ = [
    "LegacyPlacementCompatibilityAdapter", "LegacyPlacementHints",
]
