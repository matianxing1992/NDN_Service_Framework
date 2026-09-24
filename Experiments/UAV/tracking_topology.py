#!/usr/bin/env python3
"""Strict Spec191 MiniNDN topology contract.

The Controller is a process and identity inside ``gs``; it is deliberately
not represented as a sixth MiniNDN node.  ``compute`` is the only forwarding
center, so the four links below are the complete cross-node graph.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


EXPECTED_NODES = frozenset(("uav1", "uav2", "uav3", "compute", "gs"))
EXPECTED_LINKS = frozenset((
    ("uav1", "compute"),
    ("uav2", "compute"),
    ("uav3", "compute"),
    ("compute", "gs"),
))


class TopologyError(ValueError):
    """Raised when a topology violates the five-node contract."""


@dataclass(frozen=True)
class Topology:
    nodes: frozenset[str]
    links: frozenset[tuple[str, str]]
    controller_host: str = "gs"
    compute_host: str = "compute"
    link_attributes: dict[tuple[str, str], dict[str, str]] | None = None

    def validate(self) -> "Topology":
        if self.nodes != EXPECTED_NODES:
            raise TopologyError(
                f"expected exactly {sorted(EXPECTED_NODES)}, got {sorted(self.nodes)}")
        if self.links != EXPECTED_LINKS:
            raise TopologyError(
                f"expected exactly {sorted(EXPECTED_LINKS)}, got {sorted(self.links)}")
        if self.controller_host != "gs" or self.compute_host != "compute":
            raise TopologyError("Controller must run inside gs and compute must be compute")
        if self.link_attributes is not None:
            for link in EXPECTED_LINKS:
                attributes = self.link_attributes.get(link, {})
                expected = {"delay": "2ms", "bw": "100", "loss": "0"}
                if any(attributes.get(key) != value for key, value in expected.items()):
                    raise TopologyError(
                        f"link {link} must have delay=2ms bw=100 loss=0, got {attributes}")
        return self


def _section_lines(path: Path) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current: str | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
            sections[current] = []
        elif current is not None:
            sections[current].append(line)
    return sections


def load_topology(path: Path) -> Topology:
    sections = _section_lines(path)
    nodes: set[str] = set()
    for line in sections.get("nodes", []):
        if not line.endswith(":"):
            raise TopologyError(f"invalid node line: {line}")
        nodes.add(line[:-1])
    links: set[tuple[str, str]] = set()
    link_attributes: dict[tuple[str, str], dict[str, str]] = {}
    for line in sections.get("links", []):
        tokens = line.split()
        endpoints, attributes = tokens[0], tokens[1:]
        left, separator, right = endpoints.partition(":")
        if separator != ":" or not left or not right:
            raise TopologyError(f"invalid link line: {line}")
        link = (left, right)
        links.add(link)
        parsed_attributes: dict[str, str] = {}
        for token in attributes:
            if "=" not in token:
                raise TopologyError(f"invalid link attribute: {token}")
            key, value = token.split("=", 1)
            parsed_attributes[key] = value
        link_attributes[link] = parsed_attributes
    return Topology(frozenset(nodes), frozenset(links), link_attributes=link_attributes).validate()


def topology_path() -> Path:
    return Path(__file__).with_name("topology.conf")


if __name__ == "__main__":
    print(load_topology(topology_path()))
