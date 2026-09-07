"""Tests for the production-shaped rank operator seam.

These tests use lifecycle doubles only.  They prove argument binding and
fail-closed validation; they do not qualify a SIF, model, or GPU allocation.
"""
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def _layout(tmp_path, mode="local-cpu"):
    from runtime.yolo_worker import assigned_roles

    roles = assigned_roles(mode, 0)
    dirs = {}
    for name in ("bundle", "public", "output", "node", "startup", "completion", "package"):
        dirs[name] = tmp_path / name
        dirs[name].mkdir()
    homes = {}
    for role in roles:
        homes[role] = tmp_path / "homes" / role
        homes[role].mkdir(parents=True)
    plan = {
        "schema": "tiger-yolo-run-plan-v1",
        "case": mode,
        "runId": "operator-test-01",
        "applicationName": "/operator-test",
        "nodes": [{"rank": 0, "roles": list(roles)}],
        "output": str(dirs["output"]),
    }
    profile = {"timing": {"cleanupSeconds": 1}}
    startup_options = {
        "repo_free_bytes": 4096,
        "permission_wait_ms": 100,
        "network_probe_seconds": 1.0,
    }
    request_options = {
        "package": dirs["package"],
        "catalog_data_name": "/operator-test/catalogue/v1",
        "catalog_signer": "/operator-test/controller",
        "permission_wait_ms": 100,
        "request_deadline_ms": 2000,
        "process_timeout_seconds": 2.0,
        "protection_epoch": "epoch-1",
    }
    return plan, profile, dirs, homes, startup_options, request_options


def _run_kwargs(tmp_path):
    plan, profile, dirs, homes, startup, request = _layout(tmp_path)
    return dict(
        plan=plan, profile=profile, mode="local-cpu", rank=0,
        bundle=dirs["bundle"], public=dirs["public"], homes=homes,
        output=dirs["output"], node=dirs["node"],
        startup_directory=dirs["startup"], completion_directory=dirs["completion"],
        preparation_digest="sha256:" + "1" * 64,
        candidate_digest="sha256:" + "2" * 64,
        endpoints=[{"rank": 0, "address": "127.0.0.1", "port": 16380}],
        startup_seconds=5.0, completion_seconds=5.0,
        startup_options=startup, request_options=request,
        accept_request=lambda request, output: None,
    )


def test_invalid_application_name_is_rejected_before_runtime(monkeypatch, tmp_path):
    from runtime import yolo_operator

    kwargs = _run_kwargs(tmp_path)
    kwargs["plan"] = dict(kwargs["plan"], applicationName="/operator-test//sync")
    called = []
    monkeypatch.setattr(yolo_operator.NodeRuntime, "from_preparation",
                        staticmethod(lambda *args, **kwargs: called.append(True)))
    with pytest.raises(yolo_operator.OperatorError, match="OPERATOR_APPLICATION_NAME"):
        yolo_operator.run_rank(**kwargs)
    assert not called


def test_rank_operator_binds_preparation_barriers_and_lifecycle(monkeypatch, tmp_path):
    from runtime import yolo_operator

    kwargs = _run_kwargs(tmp_path)
    class FakeWorker:
        def check(self):
            return None

    worker = FakeWorker()
    prepared = {}

    def fake_prepare(plan, **values):
        prepared.update(values)
        return worker

    observed = {}

    def fake_run(worker_arg, startup, **values):
        observed.update(values)
        observed["worker"] = worker_arg
        observed["startup"] = startup
        observed["completion"] = values["completion_factory"]
        return {"status": "TEST_DOUBLE"}

    monkeypatch.setattr(yolo_operator.NodeRuntime, "from_preparation",
                        staticmethod(fake_prepare))
    import apps.yolo as yolo_app
    monkeypatch.setattr(yolo_app, "run_normal_node", fake_run)

    result = yolo_operator.run_rank(**kwargs)

    assert result == {"status": "TEST_DOUBLE"}
    assert prepared["expected_receipt_digest"] == kwargs["preparation_digest"]
    assert prepared["candidate_digest"] == kwargs["candidate_digest"]
    assert prepared["output"] == kwargs["output"]
    assert observed["worker"] is worker
    assert observed["endpoints"] == kwargs["endpoints"]
    assert observed["startup_options"] == kwargs["startup_options"]
    assert observed["request_options"] == kwargs["request_options"]
    assert observed["accept_request"] is kwargs["accept_request"]
    assert observed["allocation_expected"] is None
    startup = observed["startup"]
    completion = observed["completion"]()
    assert startup.directory == kwargs["startup_directory"]
    assert completion.directory == kwargs["completion_directory"]
    assert startup.binding == completion.binding
    assert startup.directory != completion.directory
