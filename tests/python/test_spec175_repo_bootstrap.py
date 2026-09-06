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


def test_make_backend_uses_the_current_artifact_backend_signature(
        tmp_path: Path, monkeypatch):
    helper = _module()
    token = tmp_path / "bootstrap.token"
    token.write_text("test-token\n", encoding="utf-8")
    captured = {}

    class Backend:
        @classmethod
        def from_config(cls, config, *, generated_policy_dir, state_root,
                        user, bootstrap_token, committed_receipts,
                        ack_timeout_ms, packet_payload_bytes, chunk_bytes,
                        test_only_allow_ephemeral_state_root):
            captured.update({
                "config": config,
                "generated_policy_dir": generated_policy_dir,
                "state_root": state_root,
                "user": user,
                "bootstrap_token": bootstrap_token,
                "committed_receipts": committed_receipts,
                "ack_timeout_ms": ack_timeout_ms,
                "test_only_allow_ephemeral_state_root": (
                    test_only_allow_ephemeral_state_root),
            })
            return captured

    monkeypatch.setattr(helper, "CollaborationArtifactApiBackend", Backend)
    args = SimpleNamespace(
        config="policy.yaml",
        generated_policy_dir="generated",
        state_root="state",
        user="/example/user",
        bootstrap_token_file=str(token),
        ack_timeout_ms=5000,
        test_only_allow_ephemeral_app_state=True,
    )

    result = helper.make_backend(args)

    assert result is captured
    assert captured["bootstrap_token"] == "test-token"
    assert captured["committed_receipts"] == ()
    assert captured["ack_timeout_ms"] == 5000
    assert captured["test_only_allow_ephemeral_state_root"] is True


def test_publish_receipts_are_scoped_to_the_current_operation():
    helper = _module()
    backend = SimpleNamespace(last_receipts=(
        {"receipt": {"receiptId": "old"}, "dataName": "/old"},
        {"receipt": {"receiptId": "current"}, "dataName": "/current"},
    ))
    result = SimpleNamespace(replicas=(
        SimpleNamespace(state="COMMITTED", receipt_id="current"),
    ))

    receipts = helper.receipts_for_publish_result(backend, result)

    assert receipts == [
        {"receipt": {"receiptId": "current"}, "dataName": "/current"}
    ]


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
