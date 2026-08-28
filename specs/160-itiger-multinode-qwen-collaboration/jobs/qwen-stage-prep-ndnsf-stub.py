"""Prep-only NDNSF dataclass stub for offline Qwen artifact planning.

The stage-prep job only needs to serialize the DI policy and does not execute
the real NDNSF network binding. T004 must use the real `ndnsf` module.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class CollaborationRole:
    role: str
    service: str
    artifact: str
    allow_dynamic_provisioning: bool = True
    provisioning_timeout_ms: int = 60000
    min_providers: int = 1
    max_providers: int = 1
    app_requirement: bytes = b""


@dataclass
class CollaborationDependency:
    producers: list[str] = field(default_factory=list)
    consumers: list[str] = field(default_factory=list)
    key_scope: str = ""
    topic_prefix: str = ""
    required: bool = True
