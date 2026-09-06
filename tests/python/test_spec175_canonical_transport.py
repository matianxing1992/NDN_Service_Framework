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


def _user_module():
    path = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
    spec = importlib.util.spec_from_file_location("spec175_transport_user", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


def test_tiny_v3_publisher_binds_root_to_encrypted_fetch_reference():
    user = _user_module()
    fixture = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
    manifest = json.loads((fixture / "manifest.json").read_text(encoding="utf-8"))
    rows = tuple(manifest["partitions"]["four-role"])
    digests = {
        str(row["role"]): "sha256:" + hashlib.sha256(
            (fixture / str(row["path"])).read_bytes()).hexdigest()
        for row in rows
    }
    sizes = {
        str(row["role"]): (fixture / str(row["path"])).stat().st_size
        for row in rows
    }
    from ndnsf_distributed_inference.adapters.qwen import (
        build_qwen_three_stage_adapter,
    )
    from ndnsf_distributed_inference.app_sdk.placement import ModelRef

    model = ModelRef(
        "NDNSF/Spec175TinyCausalLM",
        "sha256:" + hashlib.sha256(
            (fixture / "manifest.json").read_bytes()).hexdigest(),
        "sha256:" + "c" * 64,
        source_revision="spec175-tiny-causal-lm-v1",
    )
    adapter = build_qwen_three_stage_adapter(
        model_name=model.model_name, revision=model.source_revision,
        layer_ranges=tuple((int(row["blockStart"]), int(row["blockEndExclusive"]))
                           for row in rows),
        artifact_digests_by_role=digests, weight_bytes_by_role=sizes,
        tensor_degrees=(1,) * len(rows), precision="float32",
        adapter_name="spec175-tiny-onnx-pipeline",
        stage_roles=tuple(str(row["role"]) for row in rows),
    )
    described = adapter.describe_model(
        model.model_name, model.content_digest, model.semantics_digest,
        source_revision=model.source_revision)
    graph = adapter.graph.inspect(described)
    candidate = adapter.splitter.enumerate_candidates(described, graph)[0]

    class FakeServiceUser:
        def __init__(self):
            self.calls = []

        def publish_encrypted_large_data(self, service, payload, **kwargs):
            self.calls.append((service, bytes(payload), kwargs))
            name = f"/user/NDNSF/DI/LARGE-DATA/spec175/{len(self.calls)}/0"
            return SimpleNamespace(success=True, encrypted_data_name=name, error="")

    service_user = FakeServiceUser()
    ensurer = user._TinyCanonicalArtifactEnsurer(
        service_user=service_user, fixture_root=fixture,
        service="/LLM/Pipeline", model=model, graph=graph, candidate=candidate)
    role_specs = [
        SimpleNamespace(role=str(row["role"]), rank=0, artifact_digest=digests[str(row["role"])])
        for row in rows
    ]
    published = ensurer.ensure(candidate, role_specs, deadline_ms=10**15)

    identities = dict(published.artifact_data_names_by_role)
    fetches = dict(published.artifact_fetch_data_names_by_role)
    assert len(service_user.calls) == 2  # source, then ACTIVE root
    assert set(identities) == set(fetches) == set(digests)
    assert set(identities.values()) == {next(iter(identities.values()))}
    assert set(fetches.values()) == {next(iter(fetches.values()))}
    assert next(iter(identities.values())) != next(iter(fetches.values()))
    root = json.loads(service_user.calls[1][1].decode("utf-8"))
    assert root["metadata"]["canonicalSourceDataName"] == (
        "/user/NDNSF/DI/LARGE-DATA/spec175/1/0")
    assert root["metadata"]["canonicalSourceDigest"].startswith("sha256:")
    assert ensurer.describe(candidate).model_manifest_digest == (
        "sha256:" + hashlib.sha256(service_user.calls[1][1]).hexdigest())


def test_qwen_registration_separates_canonical_identity_from_transport_name():
    user = _user_module()
    roles = ("Stage0", "Stage1", "Stage2")
    digests = {
        role: "sha256:" + chr(ord("a") + index) * 64
        for index, role in enumerate(roles)
    }
    candidate = SimpleNamespace(
        candidate_digest="sha256:" + "f" * 64,
        execution_plan=SimpleNamespace(roles=roles),
        artifacts_by_role={role: (digest,) for role, digest in digests.items()},
    )
    publisher = user._QwenDeferredRepoPublisher(
        service_user=None,
        stage_manifest_path="/tmp/spec175-stage-manifest.json",
        output_path="/tmp/spec175-registration.json",
        publisher_identity="/publisher",
        object_prefix="/repo",
    )
    registration = {
        "artifacts": [
            {
                "role": role,
                "fileSha256": digests[role],
                "canonicalName": f"/ndnsf-di/canonical/{role}",
                "objectName": f"/repo/transport/{role}",
            }
            for role in roles
        ]
    }
    published = publisher._published(candidate, registration)
    assert dict(published.artifact_data_names_by_role) == {
        role: f"/ndnsf-di/canonical/{role}" for role in roles
    }
    assert dict(published.artifact_fetch_data_names_by_role) == {
        role: f"/repo/transport/{role}" for role in roles
    }
