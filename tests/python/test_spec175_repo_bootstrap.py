"""Regression checks for the Spec175 real-Repo bootstrap helper."""

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/spec175_repo_bootstrap.py"


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
