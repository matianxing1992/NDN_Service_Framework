"""Focused requester publication and oracle regressions; no live NFD here."""
import time
from dataclasses import replace
from types import SimpleNamespace

import pytest

from test_spec181_y_b_grant_seam import seam, build, DIGEST, EPOCH
from test_spec181_y_n_e import load_runner, _marker_spec_and_log
from ndnsf_distributed_inference.core.protected_artifacts import (
    grant_from_wire, verify_and_unwrap_grant)


@pytest.mark.parametrize("variant,reason", [
    ("EXPIRED", "key grant is expired"),
    ("WRONG_RECIPIENT", "content-key envelope failed authentication"),
    ("FORGED_AUTHORITY", "key grant authority signature is invalid"),
])
def test_actual_seam_publishes_mutation_bound_to_selection(seam, monkeypatch,
                                                         variant, reason):
    monkeypatch.setenv("SPEC180_YN_MUTATION", "Y-N-E")
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", variant)
    provider = build(seam)
    binding = provider(seam.view, int(time.time() * 1000) + 60000)
    assert len(seam.published) == 1
    name, wire = seam.published[0]
    grant = grant_from_wire(wire)
    assert name == binding.grant_name
    assert grant.grant_digest == binding.grant_digest == grant.computed_grant_digest()
    assert name.endswith("/GRANT/" + grant.grant_digest[7:])
    assert binding.provider == seam.view.provider
    assert binding.request_id == seam.view.request_id
    assert binding.attempt == seam.view.attempt
    assert binding.plan_core_digest == seam.view.plan_core_digest
    with pytest.raises(ValueError, match=reason):
        verify_and_unwrap_grant(
            grant, authority_public_key=seam.authority.public_key(),
            recipient_private_key=seam.recipient,
            expected_provider_identity=seam.view.provider,
            expected_request_id=seam.view.request_id, expected_attempt=1,
            expected_plan_core_digest=seam.view.plan_core_digest,
            expected_model_manifest_digest=DIGEST,
            expected_protection_epoch=EPOCH, now_ms=int(time.time() * 1000))


@pytest.mark.parametrize("variant", ["", "TYPO"])
def test_y_n_e_requires_explicit_registered_variant(seam, monkeypatch, variant):
    monkeypatch.setenv("SPEC180_YN_MUTATION", "Y-N-E")
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", variant)
    with pytest.raises(ValueError, match="GRANT_MUTATION_INVALID"):
        build(seam)
    assert seam.published == []


def test_y_n_e_requires_protected_epoch(seam, monkeypatch):
    monkeypatch.setenv("SPEC180_YN_MUTATION", "Y-N-E")
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", "EXPIRED")
    monkeypatch.setenv("SPEC181_PROTECTION_EPOCH", "plaintext-v1")
    with pytest.raises(ValueError, match="GRANT_MUTATION_REQUIRES_PROTECTED_EPOCH"):
        build(seam)


def test_only_one_selected_grant_is_mutated(seam, monkeypatch):
    monkeypatch.setenv("SPEC180_YN_MUTATION", "Y-N-E")
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", "EXPIRED")
    provider = build(seam)
    deadline = int(time.time() * 1000) + 60000
    provider(seam.view, deadline)
    second = replace(seam.view, request_id="req-second")
    binding = provider(second, deadline)
    assert len(seam.published) == 2
    grant = grant_from_wire(seam.published[1][1])
    assert grant.grant_digest == binding.grant_digest
    key = verify_and_unwrap_grant(
        grant, authority_public_key=seam.authority.public_key(),
        recipient_private_key=seam.recipient,
        expected_provider_identity=second.provider,
        expected_request_id=second.request_id, expected_attempt=second.attempt,
        expected_plan_core_digest=second.plan_core_digest,
        expected_model_manifest_digest=DIGEST, expected_protection_epoch=EPOCH,
        now_ms=int(time.time() * 1000))
    assert len(key) == 32


def test_user_exception_cannot_prove_provider_rejection(seam):
    journal = SimpleNamespace(last_milestone="ARTIFACTS_READY")
    assert not seam.module._spec180_negative_matches(
        "Y-N-E", ValueError("DI_PROTECTED_GRANT_REJECTED"), journal)


def test_user_pass_marker_cannot_prove_provider_rejection(tmp_path):
    runner = load_runner()
    user, log = _marker_spec_and_log(runner, tmp_path)
    marker = ("SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=Y-N-E"
              " boundary=ARTIFACTS_READY reason=DI_PROTECTED_GRANT_REJECTED"
              " requestId=/request attemptId=attempt-1 observedPhase=ARTIFACTS_READY")
    with pytest.raises(runner.RunnerError):
        runner._validate_negative_marker(marker, SimpleNamespace(name="user"),
                                         "Y-N-E", user, log)


def rejection_fixture(runner, tmp_path, variant="EXPIRED"):
    journal = runner.LifecycleJournal(tmp_path, "Y-N-E", require_protocol_binding=False)
    journal.bind_protocol_identity(request_id="/request", attempt_id="attempt-1")
    for milestone in runner.MILESTONES:
        journal.append(milestone, **({"planDigest": "sha256:" + "a" * 64}
                                     if milestone == "PLAN_SEALED" else {}))
        if milestone == "PROVIDER_EXECUTION_STARTED":
            break
    publication = {"variant": variant, "provider": "/provider/p0",
                   "requestId": "/request", "attemptId": "attempt-1",
                   "planCoreDigest": "sha256:" + "c" * 64,
                   "grantDigest": "sha256:" + "d" * 64}
    record = {key: value for key, value in publication.items() if key != "variant"}
    record.update(status="REJECTED", boundary="BEFORE_ASSEMBLY",
                  planDigest="sha256:" + "a" * 64,
                  reason=runner.YN_GRANT_REJECTIONS[variant])
    spec = SimpleNamespace(name="provider", startup_phase="providers",
                           command="native-provider --provider /provider/p0")
    user = SimpleNamespace(name="user", command="user.py --request-id /request")
    return record, publication, dict(spec=spec, user_spec=user,
                                    user_log=tmp_path / "user.log", variant=variant)


@pytest.mark.parametrize("variant", ["EXPIRED", "WRONG_RECIPIENT", "FORGED_AUTHORITY"])
def test_oracle_accepts_only_bound_provider_verifier_result(tmp_path, variant):
    runner = load_runner()
    record, publication, kwargs = rejection_fixture(runner, tmp_path, variant)
    marker = runner._validate_grant_rejection(record, publication, **kwargs)
    assert "boundary=PROVIDER_GRANT_VERIFICATION" in marker
    assert f"variant={variant}" in marker


@pytest.mark.parametrize("field,value", [
    ("status", "VERIFIED"), ("boundary", "ARTIFACTS_READY"),
    ("requestId", "/other"), ("attemptId", "attempt-2"),
    ("provider", "/other/provider"), ("planDigest", "sha256:" + "e" * 64),
    ("planCoreDigest", "sha256:" + "e" * 64),
    ("grantDigest", "sha256:" + "e" * 64),
    ("reason", "DI_PROTECTED_GRANT_REJECTED: grant acquisition cancelled"),
    ("reason", "DI_PROTECTED_GRANT_REJECTED: sealed grant reference or authority mismatch"),
])
def test_oracle_rejects_unrelated_failure_or_identity(tmp_path, field, value):
    runner = load_runner()
    record, publication, kwargs = rejection_fixture(runner, tmp_path)
    record[field] = value
    with pytest.raises(runner.RunnerError):
        runner._validate_grant_rejection(record, publication, **kwargs)


def test_oracle_rejects_user_owner_even_with_correct_data(tmp_path):
    runner = load_runner()
    record, publication, kwargs = rejection_fixture(runner, tmp_path)
    kwargs["spec"] = kwargs["user_spec"]
    with pytest.raises(runner.RunnerError, match="OWNER"):
        runner._validate_grant_rejection(record, publication, **kwargs)


def test_variant_series_preserves_existing_evidence(tmp_path, monkeypatch):
    runner = load_runner()
    target = tmp_path / "EXPIRED"
    target.mkdir()
    sentinel = target / "original.log"
    sentinel.write_text("original evidence")
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", "WRONG_RECIPIENT")
    with pytest.raises(FileExistsError):
        runner._run_y_n_e_variants(tmp_path, {})
    assert sentinel.read_text() == "original evidence"
    assert runner.os.environ["SPEC181_GRANT_MUTATION"] == "WRONG_RECIPIENT"
    assert not (tmp_path / "subcase-result.json").exists()


def test_variant_failure_prevents_later_runs_and_aggregate(tmp_path, monkeypatch):
    runner = load_runner()
    visited = []
    monkeypatch.delenv("SPEC181_GRANT_MUTATION", raising=False)

    def failed_run(case, output, inputs, *, subcase):
        visited.append(runner.os.environ["SPEC181_GRANT_MUTATION"])
        (output / "failure.log").write_text("transport failure")
        raise runner.RunnerError("unrelated transport failure")

    monkeypatch.setattr(runner, "_run_live_case_once", failed_run)
    with pytest.raises(runner.RunnerError, match="transport"):
        runner._run_y_n_e_variants(tmp_path, {})
    assert visited == ["EXPIRED"]
    assert (tmp_path / "EXPIRED/failure.log").exists()
    assert "SPEC181_GRANT_MUTATION" not in runner.os.environ
    assert not (tmp_path / "subcase-result.json").exists()
    assert not (tmp_path / "grant-mutation-matrix.json").exists()
