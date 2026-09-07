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


def test_two_rank_operator_requires_shared_probe_before_runtime(monkeypatch, tmp_path):
    from runtime import yolo_operator
    from runtime.yolo_worker import assigned_roles
    kwargs = _run_kwargs(tmp_path)
    kwargs.update(mode="two-node-gpu", allocation_expected={}, gpu_device="0")
    kwargs["plan"].update(case="two-node-gpu", nodes=[
        {"rank": rank, "roles": list(assigned_roles("two-node-gpu", rank))}
        for rank in (0, 1)])
    kwargs["endpoints"].append({"rank": 1, "address": "192.0.2.2", "port": 16380})
    def forbidden(*args, **kwargs):
        pytest.fail("missing shared probe must fail before creating a runtime")
    monkeypatch.setattr(yolo_operator.NodeRuntime, "from_preparation", forbidden)
    with pytest.raises(yolo_operator.OperatorError, match="OPERATOR_SHARED_PROBE_REQUIRED"):
        yolo_operator.run_rank(**kwargs)


def test_both_ranks_share_startup_and_completion_identity(monkeypatch, tmp_path):
    from runtime import yolo_operator
    from runtime.yolo_worker import assigned_roles
    import apps.yolo as yolo_app
    kwargs = _run_kwargs(tmp_path)
    kwargs.update(mode="two-node-gpu", allocation_expected={}, gpu_device="0",
                  probe_id="a" * 32)
    kwargs["plan"].update(case="two-node-gpu", nodes=[
        {"rank": rank, "roles": list(assigned_roles("two-node-gpu", rank))}
        for rank in (0, 1)])
    kwargs["endpoints"].append({"rank": 1, "address": "192.0.2.2", "port": 16380})
    class Worker:
        def check(self):
            pass
    monkeypatch.setattr(yolo_operator.NodeRuntime, "from_preparation", lambda *a, **k: Worker())
    barriers = []
    def lifecycle(worker, startup, **options):
        completion = options["completion_factory"]()
        startup.publish("nfd-ready", {"rank": startup.rank})
        completion.publish("workload-complete", {"rank": startup.rank})
        barriers.append((startup, completion))
        return {"rank": startup.rank}
    monkeypatch.setattr(yolo_app, "run_normal_node", lifecycle)
    for rank in (0, 1):
        yolo_operator.run_rank(**dict(kwargs, rank=rank))
    for startup, completion in barriers:
        assert startup.wait("nfd-ready") == {0: {"rank": 0}, 1: {"rank": 1}}
        assert completion.wait("workload-complete") == {0: {"rank": 0}, 1: {"rank": 1}}
        assert startup.binding == completion.binding == barriers[0][0].binding


@pytest.mark.parametrize("field,value,reason", [
    ("startup_seconds", 0, "OPERATOR_STARTUP_BUDGET"),
    ("completion_seconds", float("nan"), "OPERATOR_COMPLETION_BUDGET"),
    ("cleanup", True, "OPERATOR_CLEANUP_BUDGET"),
])
def test_all_budgets_are_validated_before_runtime(monkeypatch, tmp_path, field, value, reason):
    from runtime import yolo_operator
    kwargs = _run_kwargs(tmp_path)
    if field == "cleanup":
        kwargs["profile"]["timing"]["cleanupSeconds"] = value
    else:
        kwargs[field] = value
    def forbidden(*args, **kwargs):
        pytest.fail("invalid budget reached runtime creation")
    monkeypatch.setattr(yolo_operator.NodeRuntime, "from_preparation", forbidden)
    with pytest.raises(yolo_operator.OperatorError, match=reason):
        yolo_operator.run_rank(**kwargs)


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
