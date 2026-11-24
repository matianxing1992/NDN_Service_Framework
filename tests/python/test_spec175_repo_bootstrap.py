"""Regression checks for the Spec175 real-Repo bootstrap helper."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/spec175_repo_bootstrap.py"
NATIVE_BINDING = ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp"


def _module():
    spec = importlib.util.spec_from_file_location(
        "spec175_repo_bootstrap", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_open_repo_api_uses_owned_service_user_and_explicit_provider(monkeypatch):
    helper = _module()
    calls = []

    class User:
        def __init__(self, **kwargs):
            calls.append(kwargs)

        def start(self):
            calls.append("start")

        def stop(self):
            calls.append("stop")

    def make_persistent(user, config):
        assert isinstance(user, User)
        assert config.provider_scope == ["/example/repo"]
        assert config.service_prefix == "/NDNSF/DistributedRepo/Object/v1"
        assert config.ack_collection_time_ms == 5000
        assert config.request_timeout_ms == 60000
        calls.append("make_persistent")
        return "api"

    monkeypatch.setattr(helper, "ServiceUser", User)
    monkeypatch.setattr(helper.ArtifactRepositoryApi, "make_persistent", make_persistent)
    args = SimpleNamespace(
        group="/example/group", controller="/example/controller", user="/example/user",
        trust_schema="/test/trust.conf",
        repo_provider="/example/repo", repo_service_prefix="/NDNSF/DistributedRepo/Object/v1",
        ack_timeout_ms=5000, timeout_ms=60000,
    )
    with helper.open_repo_api(args) as api:
        assert api == "api"
        assert "bootstrap_token" not in calls[0]
        assert calls[0]["user"] == args.user
        assert calls[1:] == ["start", "make_persistent"]
    assert calls[-1] == "stop"


def test_stage_reference_is_immutable_and_belongs_to_publisher(tmp_path):
    helper = _module()
    path = tmp_path / "stage.onnx"
    path.write_bytes(b"frozen stage")
    digest = helper.sha256_file(path)
    reference = helper.stage_reference(
        SimpleNamespace(user="/example/user"), {"modelDigest": "model-revision"},
        {"stageIndex": 2, "sha256": "sha256:" + digest}, path)
    reference.validate_producer_ownership()
    assert reference.logical_name == (
        f"/example/user/NDNSF/Spec175/TinyOnnx/model-revision/stage-2/{digest}")
    assert reference.content_digest == digest
    assert reference.size_bytes == path.stat().st_size
    assert reference.policy_epoch == "repo-policy-v1"
    probe = helper.stage_reference(
        SimpleNamespace(user="/example/user"), {"modelDigest": "model-revision"},
        {"stageIndex": 2, "sha256": digest}, path, format_version="raw-file-v1")
    assert probe.content_digest == digest
    assert probe.root_manifest_name == ""


def test_publication_start_barrier_accepts_only_the_expected_token(
        tmp_path: Path):
    helper = _module()
    barrier = tmp_path / "repo-publication.start"
    barrier.write_text("expected-token\n", encoding="utf-8")
    args = SimpleNamespace(
        publication_start_barrier_file=str(barrier),
        publication_start_barrier_token="expected-token",
        publication_start_timeout_s=0.1,
    )

    helper.wait_for_publication_start(args)

    args.publication_start_barrier_token = "stale-token"
    with pytest.raises(RuntimeError, match="token mismatch"):
        helper.wait_for_publication_start(args)


def test_publication_start_barrier_arguments_are_atomic():
    helper = _module()
    args = SimpleNamespace(
        publication_start_barrier_file="/tmp/repo-publication.start",
        publication_start_barrier_token="",
        publication_start_timeout_s=0.1,
    )

    with pytest.raises(ValueError, match="provided together"):
        helper.wait_for_publication_start(args)


def test_initial_sync_wait_occurs_after_backend_creation(monkeypatch):
    helper = _module()
    events = []
    monkeypatch.setattr(helper.time, "sleep", lambda value: events.append(value))

    helper.wait_for_initial_sync(
        SimpleNamespace(initial_sync_settle_s=5.0))

    assert events == [5.0]


@pytest.mark.parametrize("value", (-0.1, 60.1))
def test_initial_sync_wait_rejects_unbounded_values(value):
    helper = _module()

    with pytest.raises(ValueError, match="between 0 and 60"):
        helper.wait_for_initial_sync(
            SimpleNamespace(initial_sync_settle_s=value))


def test_commit_plan_completion_state_cannot_dangle_after_timeout():
    """The asynchronous native commit must own its wait state past the call."""

    source = NATIVE_BINDING.read_text(encoding="utf-8")
    start = source.index("commitCollaborationPlan(")
    end = source.index("\n  py::list\n  waitForVerifiedCollaborationData", start)
    method = source[start:end]

    assert "struct CommitState" in method
    assert "std::make_shared<CommitState>()" in method
    assert "state]() mutable" in method
    assert "&mutex, &cv, &done, &result, &error" not in method


def test_commit_plan_completion_state_publishes_result_under_state_lock():
    """Result/error publication must share the wait predicate's mutex."""

    source = NATIVE_BINDING.read_text(encoding="utf-8")
    start = source.index("commitCollaborationPlan(")
    end = source.index("\n  py::list\n  waitForVerifiedCollaborationData", start)
    method = source[start:end]
    callback_start = method.index("state]() mutable")
    callback = method[callback_start:]

    lock_start = callback.index("std::lock_guard<std::mutex> lock(state->mutex)")
    lock_body = callback[lock_start:callback.index("state->cv.notify_one()", lock_start)]
    assert "state->result =" in lock_body
    assert "state->error =" in lock_body
