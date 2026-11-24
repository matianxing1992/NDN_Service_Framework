"""Regression tests for Spec175 canonical identity/transport separation."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys


ROOT = Path(__file__).resolve().parents[2]
for relative in (
    "NDNSF-DistributedInference",
    "NDNSF-DistributedRepo/pythonWrapper",
    "pythonWrapper",
    "examples/python/NDNSF-DistributedInference/llm_pipeline",
):
    path = str(ROOT / relative)
    if path not in sys.path:
        sys.path.insert(0, path)


def test_published_split_defaults_fetch_reference_for_legacy_callers():
    from ndnsf_distributed_inference.app_sdk.placement import PublishedSplit

    value = PublishedSplit(
        candidate_digest="sha256:" + "a" * 64,
        artifact_digests_by_role={"role": "sha256:" + "b" * 64},
        artifact_data_names_by_role={"role": "/canonical/root"},
    )
    assert dict(value.artifact_fetch_data_names_by_role) == {
        "role": "/canonical/root"
    }
