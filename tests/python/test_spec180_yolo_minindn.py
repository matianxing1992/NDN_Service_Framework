from __future__ import annotations

import json

import importlib.util
from pathlib import Path
import sqlite3
import sys
from types import SimpleNamespace

import pytest

from ndnsf_distributed_inference.app_sdk.contracts import PreSplitCatalogSnapshot


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("spec180_yolo_minindn", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_minindn_environment_compat_preserves_equals_in_both_entrypoints():
    module = load_runner()

    class Completed:
        def communicate(self):
            return (b"A=one=two\nB=plain\nNO_EQUALS\n", b"")

    class Node:
        params = {"params": {"homeDir": "/tmp/fake-minindn-node"}}

        def __init__(self):
            self.calls = []

        def popen(self, argv, cwd=None, **kwargs):
            self.calls.append((argv, cwd, kwargs))
            if argv != ["printenv"]:
                return SimpleNamespace()
            assert cwd == self.params["params"]["homeDir"]
            return Completed()

    module._install_minindn_environment_compat()
    import minindn.apps.application as application
    import minindn.util as util

    expected = {
        "A": "one=two",
        "B": "plain",
        "HOME": "/tmp/fake-minindn-node",
        "C": "value=with=equals",
    }
    assert util.popenGetEnv(Node(), {"C": "value=with=equals"}) == expected
    application_node = Node()
    application.getPopen(application_node, "echo command",
                          envDict={"C": "value=with=equals"}, shell=True)
    assert application_node.calls[-1][0] == "echo command"
    assert application_node.calls[-1][1] == "/tmp/fake-minindn-node"
    assert application_node.calls[-1][2]["env"] == expected


def _negative_children(module, tmp_path, *, case="Y-N-P", marker_request="/request",
                       owner="user", sibling_exit=None):
    phase = "PROVIDER_EXECUTION_STARTED" if case == "Y-N-I" else "GRAPH_READY"
    journal = module.LifecycleJournal(tmp_path, case, require_protocol_binding=True)
    journal.bind_protocol_identity(request_id="/request", attempt_id="attempt-1")
    for milestone in module.MILESTONES:
        journal.append(milestone, **({"planDigest": "sha256:" + "a" * 64}
                                    if milestone == "PLAN_SEALED" else {}))
        if milestone == phase:
            break
    marker = ("SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=" + case
              + " boundary=" + module.YN_SUBCASE_BOUNDARIES[case]
              + " reason=" + module.YN_NEGATIVE_REASONS[case]
              + " requestId=" + marker_request
              + " attemptId=attempt-1 observedPhase=" + phase)
    result = []
    for name, exit_code in [(owner, None), ("controller", sibling_exit)]:
        path = tmp_path / (name + ".log")
        path.write_text(marker + "\n" if name == owner else "")
        spec = module.CaseProcessSpec(name, "node", "python user.py --request-id /request",
                                      "", "user" if name == "user" else "controller")
        result.append((spec, SimpleNamespace(poll=lambda code=exit_code: code), path))
    return tuple(result)


def test_negative_runner_requires_current_request_identity(tmp_path):
    module = load_runner()
    started = _negative_children(module, tmp_path, marker_request="/previous-request")
    with pytest.raises(module.RunnerError, match="IDENTITY"):
        module._wait_for_negative_result(started, "Y-N-P", 0.01)


def test_negative_runner_accepts_bound_semantic_rejection(tmp_path):
    module = load_runner()
    started = _negative_children(module, tmp_path)
    assert "requestId=/request" in module._wait_for_negative_result(started, "Y-N-P", 0.01)


def test_negative_runner_checks_sibling_crash_even_after_marker(tmp_path):
    module = load_runner()
    started = _negative_children(module, tmp_path, sibling_exit=17)
    with pytest.raises(module.RunnerError, match="CHILD_FAILURE"):
        module._wait_for_negative_result(started, "Y-N-P", 0.01)


def test_input_negative_cannot_be_attested_by_user(tmp_path):
    module = load_runner()
    started = _negative_children(module, tmp_path, case="Y-N-I")
    with pytest.raises(module.RunnerError, match="OWNER"):
        module._wait_for_negative_result(started, "Y-N-I", 0.01)


@pytest.mark.parametrize("plan", ["a", "b"])
def test_input_negative_binds_provider_and_sealed_plan(tmp_path, plan):
    module = load_runner()
    started = list(_negative_children(module, tmp_path, case="Y-N-I"))
    user_log = started[0][2]
    marker = user_log.read_text().strip()
    user_log.write_text("")
    provider_log = tmp_path / "provider.log"
    provider_log.write_text(marker + " provider=/example/provider/Shard planDigest=sha256:"
                            + plan * 64 + " errorCode=DI_INPUT_FETCH_ROLE_MISMATCH\n")
    spec = module.CaseProcessSpec("provider-Shard", "node",
                                  "di-native-provider --provider /example/provider/Shard",
                                  "", "providers")
    started.append((spec, SimpleNamespace(poll=lambda: None), provider_log))
    if plan == "a":
        assert "provider=/example/provider/Shard" in module._wait_for_negative_result(
            tuple(started), "Y-N-I", 0.01)
    else:
        with pytest.raises(module.RunnerError, match="PROVIDER_BINDING_MISMATCH"):
            module._wait_for_negative_result(tuple(started), "Y-N-I", 0.01)


def test_missing_candidate_environment_fails_before_output_creation(tmp_path: Path):
    module = load_runner()
    output = tmp_path / "case-output"
    with pytest.raises(module.RunnerError, match="ENVIRONMENT_MISSING"):
        module.validate_inputs("Y-A", {
            "SPEC180_CASE_OUTPUT_DIR": str(output),
        })
    assert not output.exists()


def test_missing_case_output_root_is_rejected(tmp_path: Path):
    module = load_runner()
    with pytest.raises(module.RunnerError, match="OUTPUT_ROOT_MISSING"):
        module._validate_output_root(str(tmp_path / "missing"))


def test_state_root_must_be_persistent_and_absolute(tmp_path: Path):
    module = load_runner()
    persistent = Path.home() / ".ndnsf-spec180-test-state"
    assert module._validate_state_root(str(persistent)) == persistent.resolve()
    with pytest.raises(module.RunnerError, match="STATE_ROOT_NOT_ABSOLUTE"):
        module._validate_state_root("relative/state")
    with pytest.raises(module.RunnerError, match="STATE_ROOT_VOLATILE"):
        module._validate_state_root("/tmp/spec180-state")


def test_state_root_rejects_existing_directory_owned_by_another_identity(
        tmp_path: Path, monkeypatch):
    module = load_runner()
    persistent = Path.home() / (".ndnsf-spec180-owner-" + tmp_path.name)
    persistent.mkdir(mode=0o700)
    try:
        owner = persistent.stat().st_uid
        monkeypatch.setattr(module.os, "geteuid", lambda: owner + 1)
        with pytest.raises(module.RunnerError,
                           match="STATE_ROOT_OWNER_MISMATCH"):
            module._validate_state_root(str(persistent))
    finally:
        persistent.rmdir()


def test_native_library_closure_rejects_split_ndn_cxx(monkeypatch):
    module = load_runner()
    monkeypatch.setattr(module.shutil, "which", lambda name: "/usr/local/bin/nfd")
    extension = next((module.ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    ndn_svs = module.ROOT / "ndnsf-svs-test.so"
    nac_abe = module.ROOT / "nac-abe-test.so"
    monkeypatch.setattr(module, "_ldd_library_map", lambda path: {
        "libndn-cxx.so.0.9.0": Path("/opt/" +
                                     ("local/libndn-cxx.so.0.9.0"
                                      if Path(path) == extension
                                      else "system/libndn-cxx.so.0.9.0")),
        "libndn-svs.so.0.1.0": ndn_svs,
        "libnac-abe.so": nac_abe,
    } if Path(path) == extension else {
        "libndn-cxx.so.0.9.0": Path("/opt/system/libndn-cxx.so.0.9.0"),
    })
    monkeypatch.setattr(module, "digest_file", lambda path: str(path))
    with pytest.raises(module.RunnerError,
                       match="NATIVE_LIBRARY_CLOSURE_MISMATCH"):
        module._validate_native_library_closure()


def test_native_library_closure_rejects_historical_local_prefix(monkeypatch):
    module = load_runner()
    monkeypatch.setattr(module.shutil, "which", lambda name: "/usr/local/bin/nfd")
    extension = next((module.ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    legacy = (module.ROOT / ".local-boost171/lib/libndn-cxx.so.0.9.0").resolve()
    monkeypatch.setattr(module, "_ldd_library_map", lambda path: {
        "libndn-cxx.so.0.9.0": legacy,
    })
    monkeypatch.setattr(module, "digest_file", lambda path: str(path))
    with pytest.raises(module.RunnerError,
                       match="NATIVE_LIBRARY_CLOSURE_LEGACY_LOCAL_PREFIX"):
        module._validate_native_library_closure()


@pytest.mark.parametrize("output, error", [
    ("libmissing.so => not found\n", "NATIVE_LIBRARY_CLOSURE_UNRESOLVED"),
    ("libbroken.so ??? /tmp/broken (0x1234)\n",
     "NATIVE_LIBRARY_CLOSURE_UNRECOGNIZED"),
])
def test_native_library_closure_ldd_parser_is_fail_closed(monkeypatch, tmp_path,
                                                            output, error):
    module = load_runner()

    class Result:
        returncode = 0
        stdout = output

    monkeypatch.setattr(module.subprocess, "run", lambda *args, **kwargs: Result())
    with pytest.raises(module.RunnerError, match=error):
        module._ldd_library_map(tmp_path / "fixture.so")


def test_host_build_guard_rejects_missing_receipt(tmp_path):
    module = load_runner()
    with pytest.raises(module.RunnerError, match="LOCAL_MANIFEST_MISSING"):
        module._validate_local_native_build({
            "SPEC180_NATIVE_BUILD_MANIFEST": str(tmp_path / "missing.json")})


def test_sif_never_uses_host_build_receipt(monkeypatch):
    module = load_runner()
    monkeypatch.setattr(module, "SIF_RUNTIME_SIF", "/sealed/candidate.sif")
    monkeypatch.setattr(module.importlib.util, "spec_from_file_location",
                        lambda *args: pytest.fail("host guard used for SIF"))
    module._validate_local_native_build({})


@pytest.mark.parametrize("case,subcase,mutation", [
    ("Y-A", "", ""),
    *[("Y-N", name, "") for name in
      ("Y-N-O", "Y-N-C", "Y-N-P", "Y-N-R", "Y-N-I", "Y-N-L")],
    ("Y-B", "", ""),
    *[("Y-N", "Y-N-E", value) for value in
      ("EXPIRED", "WRONG_RECIPIENT", "FORGED_AUTHORITY")],
])
def test_live_case_child_epoch_matches_publication(monkeypatch, tmp_path,
                                                  case, subcase, mutation):
    module = load_runner()
    requested = "spec180-yolo-protected-v1"
    monkeypatch.setenv(module.PROTECTION_EPOCH_ENV, requested)
    monkeypatch.setenv("SPEC181_GRANT_MUTATION", mutation)
    monkeypatch.setenv("NDNSF_DI_ENVELOPE_KEY_FILE", str(tmp_path / "requester.key"))
    monkeypatch.setenv("SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP", str(tmp_path / "keys.json"))
    monkeypatch.setenv("NDNSF_SPEC180_CONFIG_ROOT", str(tmp_path))
    (tmp_path / "artifact-policy-authority.key").write_text("preflight fixture")
    publication_inputs, child_environment = {}, {}
    def binding(_case, _output, inputs):
        publication_inputs.update(inputs)
        return SimpleNamespace(output=tmp_path)
    monkeypatch.setattr(module, "CaseRuntimeBinding", SimpleNamespace(from_inputs=binding))
    monkeypatch.setattr(module, "build_runtime_publication_file", lambda *args: tmp_path)
    monkeypatch.setattr(module, "MiniNdnCaseRuntime", lambda *args: SimpleNamespace(
        process_specs=lambda: None,
        start_network=lambda: pytest.fail("network must not start in focused test"),
        stop=lambda: None))
    monkeypatch.setattr(module, "_validate_native_library_closure", lambda: None)
    def stop_before_network(env):
        child_environment.update(env)
        raise module.RunnerError("EPOCH_CAPTURED")
    monkeypatch.setattr(module, "_validate_local_native_build", stop_before_network)
    with pytest.raises(module.RunnerError, match="EPOCH_CAPTURED"):
        module._run_live_case_once(case, tmp_path, {}, subcase=subcase)
    expected = requested if case == "Y-B" or subcase == "Y-N-E" else module.PLAINTEXT_EPOCH
    assert publication_inputs.get("protection_epoch", module.PLAINTEXT_EPOCH) == expected
    assert child_environment[module.PROTECTION_EPOCH_ENV] == expected
    assert module.os.environ[module.PROTECTION_EPOCH_ENV] == requested
    if expected == requested:
        assert child_environment["SPEC181_REQUESTER_PRIVATE_KEY"]
        assert child_environment["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"]


def test_live_case_build_rejection_has_no_network_side_effect(monkeypatch, tmp_path):
    module = load_runner()
    calls = []
    monkeypatch.setattr(module, "CaseRuntimeBinding", SimpleNamespace(
        from_inputs=lambda *args: SimpleNamespace(output=tmp_path)))
    monkeypatch.setattr(module, "build_runtime_publication_file", lambda *args: tmp_path)
    monkeypatch.setattr(module, "MiniNdnCaseRuntime", lambda *args: SimpleNamespace(
        process_specs=lambda: calls.append("preflight"),
        start_network=lambda: calls.append("network"),
        stop=lambda: calls.append("cleanup")))
    monkeypatch.setattr(module, "_validate_native_library_closure", lambda: None)
    def reject(env):
        raise module.RunnerError("LOCAL_NATIVE_BUILD_REJECTED:STALE_SOURCES")
    monkeypatch.setattr(module, "_validate_local_native_build", reject)
    with pytest.raises(module.RunnerError, match="STALE_SOURCES"):
        module._run_live_case_once("Y-A", tmp_path, {})
    assert calls == ["preflight", "cleanup"]


@pytest.mark.parametrize("user_exit,other_exit,after_stop,expected", [
    (91, None, -2, None),
    (1, None, -2, "CHILD_FAILURE"),
    (91, 17, -2, "CHILD_FAILURE"),
    (91, None, -9, "CLEANUP_FAILURE"),
    (91, None, None, "CLEANUP_FAILURE"),
    (None, None, -2, "USER_DID_NOT_EXIT"),
])
def test_terminal_cleanup_requires_real_user_exit_and_all_children(
        tmp_path, user_exit, other_exit, after_stop, expected):
    module = load_runner()
    state = {"user": user_exit, "provider": other_exit}
    started = [(SimpleNamespace(name=name),
                SimpleNamespace(poll=lambda name=name: state[name]), tmp_path / name)
               for name in state]
    runtime = SimpleNamespace(stop=lambda: state.update(provider=after_stop))
    if expected:
        with pytest.raises(module.RunnerError, match=expected):
            module._close_case_children(runtime, started, expected_user_exit=91,
                                        timeout_s=0.01)
    else:
        result = module._close_case_children(runtime, started, expected_user_exit=91)
        assert result[0]["terminationRequested"] is False
        assert result[1]["exitStatus"] == -2


def test_catalog_identity_is_explicit_and_name_bound():
    module = load_runner()
    valid = {
        "SPEC180_YOLO_CATALOG_DATA_NAME": "/example/controller/NDNSF/DI/catalogue/v1",
        "SPEC180_YOLO_CATALOG_SIGNER": "/example/controller",
    }
    assert module._validate_catalog_identity(valid) == (
        "/example/controller/NDNSF/DI/catalogue/v1", "/example/controller")
    with pytest.raises(module.RunnerError, match="CATALOG_DATA_NAME_INVALID"):
        module._validate_catalog_identity({
            **valid, "SPEC180_YOLO_CATALOG_DATA_NAME": "relative/name"})
    with pytest.raises(module.RunnerError, match="CATALOG_SIGNER_INVALID"):
        module._validate_catalog_identity({
            **valid, "SPEC180_YOLO_CATALOG_SIGNER": "signer with spaces"})
    with pytest.raises(module.RunnerError,
                       match="CATALOG_DATA_NAME_OUTSIDE_DI_PREFIX"):
        module._validate_catalog_identity({
            **valid, "SPEC180_YOLO_CATALOG_DATA_NAME": "/spec180/catalogue/v1"})
    with pytest.raises(module.RunnerError,
                       match="CATALOG_SIGNER_DATA_PREFIX_MISMATCH"):
        module._validate_catalog_identity({
            **valid, "SPEC180_YOLO_CATALOG_SIGNER": "/example/other"})


def test_manifest_secret_scan_is_recursive():
    module = load_runner()
    assert module._contains_secret({"catalogue": {"entries": [{
        "privateKey": "must reject",
    }]}})
    assert not module._contains_secret({"catalogue": {"candidateId": "atomic-v1"}})


def test_package_object_paths_cannot_escape_package_root(tmp_path: Path):
    module = load_runner()
    package = tmp_path / "package"
    package.mkdir()
    outside = tmp_path / "outside.weights"
    outside.write_bytes(b"weights")
    with pytest.raises(module.RunnerError, match="PACKAGE_PATH_ESCAPES_ROOT"):
        module._package_file(package, "../outside.weights", "weights")


def test_journal_enforces_exact_order_and_no_secret_fields(tmp_path: Path):
    module = load_runner()
    journal = module.LifecycleJournal(tmp_path, "Y-A",
                                      require_protocol_binding=False)
    journal.append("INPUT_REFERENCE_PUBLISHED", referenceDigest="sha256:" + "a" * 64)
    with pytest.raises(module.RunnerError, match="OUT_OF_ORDER"):
        journal.append("ACK_CLOSED")
    journal.append("REQUEST_SENT")
    with pytest.raises(module.RunnerError, match="SECRET_FIELD"):
        journal.append("ACK_CLOSED", plaintext="must-not-be-written")
    assert journal.path.read_text(encoding="utf-8").count("\n") == 2
    records = [__import__("json").loads(line)
               for line in journal.path.read_text(encoding="utf-8").splitlines()]
    assert records[0]["caseId"] == "Y-A"
    assert "case" not in records[0]
    assert records[0]["requestId"] == journal.request_id
    assert records[0]["attemptId"] == journal.attempt_id
    assert records[0]["attemptId"].startswith("attempt-")


def test_journal_rejects_generic_payload_and_unknown_fields(tmp_path: Path):
    module = load_runner()
    journal = module.LifecycleJournal(tmp_path, "Y-A",
                                      require_protocol_binding=False)
    with pytest.raises(module.RunnerError, match="FIELD_FORBIDDEN:payload"):
        journal.append("INPUT_REFERENCE_PUBLISHED", payload="opaque-data")
    with pytest.raises(module.RunnerError, match="FIELD_FORBIDDEN:unexpected"):
        journal.append("INPUT_REFERENCE_PUBLISHED", unexpected="value")
    assert not journal.path.exists()


def test_journal_accepts_only_scalar_milestone_summaries(tmp_path: Path):
    module = load_runner()
    journal = module.LifecycleJournal(tmp_path, "Y-A",
                                      require_protocol_binding=False)
    journal.append("INPUT_REFERENCE_PUBLISHED",
                   referenceDigest="sha256:" + "a" * 64)
    with pytest.raises(module.RunnerError, match="FIELD_FORBIDDEN:requestDigest"):
        journal.append("REQUEST_SENT", requestDigest={"sha256": "a"})
    journal.append("REQUEST_SENT", requestDigest="sha256:" + "b" * 64)
    assert journal.path.read_text(encoding="utf-8").count("\n") == 2


def test_journal_rejects_duplicate_milestone(tmp_path: Path):
    module = load_runner()
    journal = module.LifecycleJournal(tmp_path, "Y-B",
                                      require_protocol_binding=False)
    journal.append("INPUT_REFERENCE_PUBLISHED")
    with pytest.raises(module.RunnerError, match="DUPLICATE"):
        journal.append("INPUT_REFERENCE_PUBLISHED")


def test_journal_requires_all_milestones_before_completion(tmp_path: Path):
    module = load_runner()
    journal = module.LifecycleJournal(tmp_path, "Y-A",
                                      require_protocol_binding=False)
    with pytest.raises(module.RunnerError, match="INCOMPLETE"):
        journal.validate_complete()


def test_live_journal_requires_coordinator_request_and_attempt_binding(tmp_path: Path):
    module = load_runner()
    # Production default is fail-closed; only schema fixtures opt out below.
    journal = module.LifecycleJournal(tmp_path, "Y-A")
    with pytest.raises(module.RunnerError, match="IDENTITY_UNBOUND"):
        journal.append("INPUT_REFERENCE_PUBLISHED")
    journal.bind_protocol_identity(
        request_id="request-42", attempt_id="attempt-7")
    journal.append("INPUT_REFERENCE_PUBLISHED")
    assert journal.request_id == "request-42"
    assert journal.attempt_id == "attempt-7"
    with pytest.raises(module.RunnerError, match="IDENTITY_REBIND"):
        journal.bind_protocol_identity(
            request_id="request-43", attempt_id="attempt-8")

    absolute = module.LifecycleJournal(tmp_path / "absolute", "Y-A")
    absolute.bind_protocol_identity(
        request_id="/spec180-y-a-request-42", attempt_id="attempt-1")
    assert absolute.request_id == "/spec180-y-a-request-42"


def test_live_journal_binds_returned_coordinator_handle(tmp_path: Path):
    module = load_runner()

    class Collaboration:
        request_id = "coordinator-request"

    class Handle:
        collaboration = Collaboration()

    journal = module.LifecycleJournal(tmp_path, "Y-A")
    journal.bind_coordinator_handle(Handle(), attempt_id="attempt-1")
    journal.append("INPUT_REFERENCE_PUBLISHED",
                   referenceDigest="sha256:" + "a" * 64)
    assert journal.request_id == "coordinator-request"
    assert journal.attempt_id == "attempt-1"
    with pytest.raises(module.RunnerError,
                       match="COORDINATOR_REQUEST_ID_MISSING"):
        module.LifecycleJournal(tmp_path / "missing", "Y-A").bind_coordinator_handle(
            object(), attempt_id="attempt-1")


def test_main_never_emits_pass_when_real_driver_is_not_wired(monkeypatch, tmp_path: Path,
                                                             capsys):
    module = load_runner()

    def fake_validate(case, environment):
        return tmp_path, {"case": case}

    monkeypatch.setattr(module, "validate_inputs", fake_validate)
    monkeypatch.setattr(module, "_validate_native_library_closure", lambda: None)
    rc = module.main(["--case", "Y-A"])
    captured = capsys.readouterr().out
    assert rc == 2
    assert "status=PASS" not in captured
    assert "status=UNQUALIFIED" in captured


def test_main_reports_missing_g0_inputs_as_waiting_external_input(monkeypatch,
                                                                   capsys):
    module = load_runner()
    for name in module.REQUIRED_ENV:
        monkeypatch.delenv(name, raising=False)
    rc = module.main(["--case", "Y-A"])
    captured = capsys.readouterr().out
    assert rc == 78
    assert "status=WAITING_EXTERNAL_INPUT" in captured
    assert "status=UNQUALIFIED" not in captured


def test_y_n_matrix_is_fixed_and_ordered():
    module = load_runner()
    assert module.YN_SUBCASES == (
        "Y-N-O", "Y-N-C", "Y-N-P", "Y-N-R", "Y-N-I", "Y-N-E", "Y-N-L",
    )
    assert module.MILESTONES.index("INPUT_REFERENCE_PUBLISHED") < module.MILESTONES.index(
        "ACK_CLOSED") < module.MILESTONES.index("TERMINAL_RESPONSE")


def _candidate(candidate_id: str, roles: tuple[str, ...], *, priority: int,
               ingress: str, egress: str) -> dict:
    digest = ("sha256:" + "a" * 64
              if candidate_id == "atomic-v1"
              else "sha256:" + "b" * 64)
    return {
        "candidateId": candidate_id,
        "candidateDigest": digest,
        "selectionPriority": priority,
        "roles": [{"role": role, "kind": "COMPONENT_SET"} for role in roles],
        "inputIngressRole": ingress,
        "resultEgressRole": egress,
        "mergeKind": "NATIVE_POSTPROCESS",
    }


def _case_manifest() -> dict:
    return {
        "catalogue": {
            "candidates": [
                _candidate("atomic-v1", ("FullModel",), priority=10,
                           ingress="FullModel", egress="FullModel"),
                _candidate(
                    "shared-backbone-two-shard-v1",
                    ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"),
                    priority=20, ingress="BackboneNeck", egress="Merge",
                ),
            ],
        },
    }


def test_case_plan_binds_expected_roles_without_provider_assignment(tmp_path: Path):
    module = load_runner()
    plan = module._build_case_plan(
        "Y-B", _case_manifest(), "sha256:" + "b" * 64)
    assert plan["schema"] == "spec180-yolo-case-plan-v1"
    assert plan["candidateAuthority"] == "ACK_SNAPSHOT_ONLY"
    assert plan["providerAssignment"] == "RUNTIME_ACK_PROJECTION_REQUIRED"
    assert plan["candidatePlans"][0]["roles"] == [
        "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"
    ]
    assert all("provider" not in key.lower()
               for key in plan["candidatePlans"][0])


def test_case_plan_y_n_records_fixed_subcase_outcomes():
    module = load_runner()
    plan = module._build_case_plan(
        "Y-N", _case_manifest(), "sha256:" + "b" * 64)
    assert [item["id"] for item in plan["subcases"]] == list(module.YN_SUBCASES)
    assert plan["subcases"][0]["expected"] == "CONTROL"
    assert all(item["expected"] == "FAIL_CLOSED" for item in plan["subcases"][1:])
    assert [item["boundary"] for item in plan["subcases"]] == [
        module.YN_SUBCASE_BOUNDARIES[item] for item in module.YN_SUBCASES
    ]


def test_y_n_focused_probes_cannot_qualify_provider_grants(tmp_path: Path):
    module = load_runner()
    cases = [item for item in module.YN_SUBCASES[1:] if item != "Y-N-E"]
    results = [
        module._run_focused_y_n_negative(subcase, tmp_path, {})
        for subcase in cases
    ]
    assert [item["subcaseId"] for item in results] == cases
    assert all(item["status"] == "PASS" for item in results)
    assert all(item["outcome"] == "FAIL_CLOSED" for item in results)
    assert [item["boundary"] for item in results] == [
        module.YN_SUBCASE_BOUNDARIES[item] for item in cases
    ]
    assert all((tmp_path / "subcases" / item / "subcase-result.json").is_file()
               for item in cases)
    with pytest.raises(module.RunnerError, match="PRODUCTION_VERIFIER_REQUIRED"):
        module._run_focused_y_n_negative("Y-N-E", tmp_path, {})


def test_y_n_qualification_matrix_dispatches_every_negative_to_live_runner(
        tmp_path: Path, monkeypatch):
    module = load_runner()
    observed = []

    def fake_live(case, output, _inputs, *, subcase=""):
        observed.append((case, subcase, output.name))
        if subcase == "Y-N-O":
            outcome, reason = "CONTROL", "TERMINAL_RESPONSE_VERIFIED"
            status = "PASS"
        else:
            # spec181 T006: Y-N-E is a registered negative subcase like the
            # rest — the live child must record a real mutation rejection.
            outcome, reason = "FAIL_CLOSED", module.YN_NEGATIVE_REASONS[subcase]
            status = "PASS"
        module._write_subcase_result(
            output, subcase=subcase, status=status, outcome=outcome,
            reason=reason, child_count=7)
        if subcase == "Y-N-E":
            (output / "negative-evidence.json").write_text(module.json.dumps({
                "variant": output.name, "status": "PASS", "reason": reason}))
        return 0

    def focused_probe_must_not_be_used(*_args, **_kwargs):
        raise AssertionError("qualification matrix used an offline focused probe")

    monkeypatch.setattr(module, "_run_live_case_once", fake_live)
    monkeypatch.setattr(module, "_run_focused_y_n_negative",
                        focused_probe_must_not_be_used)

    # Every negative dispatches to the live runner; with the T006 real
    # mutations every row is a registered PASS, so the matrix completes.
    assert module._run_y_n_matrix(tmp_path, {}) == 0
    assert observed == [("Y-N", subcase, name)
                        for subcase in module.YN_SUBCASES
                        for name in (tuple(module.YN_GRANT_REJECTIONS)
                                     if subcase == "Y-N-E" else (subcase,))]
    matrix = module.json.loads(
        (tmp_path / "y-n-matrix-result.json").read_text(encoding="utf-8"))
    assert matrix["aggregate"] == "PASS"
    rows = {item["id"]: item for item in matrix["subcases"]}
    assert rows["Y-N-E"]["status"] == "PASS"
    assert rows["Y-N-E"]["outcome"] == "FAIL_CLOSED"
    assert rows["Y-N-O"]["status"] == "PASS"
    assert all(rows[item]["status"] == "PASS" for item in
               module.YN_SUBCASES[1:] if item != "Y-N-E")


def test_y_n_subcase_output_must_be_fresh(tmp_path: Path):
    module = load_runner()
    first = module._new_y_n_subcase_dir(tmp_path, "Y-N-P")
    assert first.is_dir()
    with pytest.raises(module.RunnerError, match="OUTPUT_NOT_FRESH:Y-N-P"):
        module._new_y_n_subcase_dir(tmp_path, "Y-N-P")


def test_y_n_aggregate_pass_requires_live_order_control(tmp_path: Path, monkeypatch):
    module = load_runner()

    def fail_control(*_args, **_kwargs):
        raise module.RunnerError("simulated control failure")

    monkeypatch.setattr(module, "_run_live_case_once", fail_control)
    with pytest.raises(module.RunnerError, match="Y_N_MATRIX_INCOMPLETE:Y-N-O"):
        module._run_y_n_matrix(tmp_path, {})
    assert not (tmp_path / "y-n-matrix-result.json").exists()
    assert (tmp_path / "subcases" / "Y-N-O" / "subcase-result.json").is_file()


def test_case_plan_rejects_role_set_or_missing_candidate():
    module = load_runner()
    manifest = _case_manifest()
    manifest["catalogue"]["candidates"][1]["roles"] = [
        {"role": "BackboneNeck", "kind": "COMPONENT_SET"}
    ]
    with pytest.raises(module.RunnerError, match="ROLE_SET_INVALID"):
        module._build_case_plan("Y-B", manifest, "sha256:" + "b" * 64)
    with pytest.raises(module.RunnerError, match="CANDIDATE_MISSING"):
        module._build_case_plan("Y-A", {"catalogue": {"candidates": []}},
                                "sha256:" + "b" * 64)


def test_runtime_catalogue_payload_requires_case_candidate_coverage():
    module = load_runner()
    digest = "sha256:" + "b" * 64
    snapshot = PreSplitCatalogSnapshot(
        alias="yolo-shared",
        manifest_digest=digest,
        model_content_digest=digest,
        semantics_digest=digest,
        graph_digest=digest,
        candidate_digest=digest,
        backend="onnxruntime-cpu",
        precision="float32",
        artifact_data_names={
            role: ("/repo/segments/" + role.lower(),)
            for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
        },
        status="ACTIVE",
        created_at_ms=1,
    )
    payload = module.build_runtime_catalogue_payload(
        "Y-B", "/example/controller/NDNSF/DI/catalogue/v1",
        _case_manifest(), [snapshot])
    envelope = __import__("json").loads(payload.decode("utf-8"))
    assert envelope["schema"] == "ndnsf-di-presplit-catalog-snapshot-v1"
    assert envelope["recordName"].startswith("/example/controller/NDNSF/DI/")
    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_SNAPSHOT_CANDIDATE_COVERAGE"):
        module.build_runtime_catalogue_payload(
            "Y-B", "/example/controller/NDNSF/DI/catalogue/v1",
            _case_manifest(), [])


def test_runtime_catalogue_payload_requires_exact_role_artifact_coverage():
    module = load_runner()
    digest = "sha256:" + "b" * 64
    snapshot = PreSplitCatalogSnapshot(
        alias="yolo-shared",
        manifest_digest=digest,
        model_content_digest=digest,
        semantics_digest=digest,
        graph_digest=digest,
        candidate_digest=digest,
        backend="onnxruntime-cpu",
        precision="float32",
        artifact_data_names={"BackboneNeck": ("/repo/segments/backbone",)},
        status="ACTIVE",
        created_at_ms=1,
    )
    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_SNAPSHOT_ARTIFACT_COVERAGE"):
        module.build_runtime_catalogue_payload(
            "Y-B", "/example/controller/NDNSF/DI/catalogue/v1",
            _case_manifest(), [snapshot])


def test_runtime_catalogue_payload_rejects_unexpected_or_duplicate_artifacts():
    module = load_runner()
    digest = "sha256:" + "b" * 64
    roles = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")

    def make_snapshot(names):
        return PreSplitCatalogSnapshot(
            alias="yolo-shared",
            manifest_digest=digest,
            model_content_digest=digest,
            semantics_digest=digest,
            graph_digest=digest,
            candidate_digest=digest,
            backend="onnxruntime-cpu",
            precision="float32",
            artifact_data_names=names,
            status="ACTIVE",
            created_at_ms=1,
        )

    duplicate = make_snapshot({role: ("/repo/segments/shared",)
                               for role in roles})
    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_SNAPSHOT_ARTIFACT_DUPLICATE"):
        module.build_runtime_catalogue_payload(
            "Y-B", "/example/controller/NDNSF/DI/catalogue/v1",
            _case_manifest(), [duplicate])

    unexpected = make_snapshot({
        **{role: ("/repo/segments/" + role.lower(),) for role in roles},
        "UnexpectedRole": ("/repo/segments/unexpected",),
    })
    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_SNAPSHOT_ARTIFACT_COVERAGE"):
        module.build_runtime_catalogue_payload(
            "Y-B", "/example/controller/NDNSF/DI/catalogue/v1",
            _case_manifest(), [unexpected])


def _case_config(case: str = "Y-B") -> dict:
    roles = {
        "Y-A": ["FullModel"],
        "Y-B": ["BackboneNeck", "DetectShard0", "DetectShard1", "Merge"],
        "Y-N": ["FullModel", "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"],
    }[case]
    if case == "Y-N":
        # Four identities are enough for both registered candidates: the
        # first advertises FullModel and BackboneNeck, and the remaining
        # identities advertise one shared role each.  This is a capability
        # witness, not a request-time assignment.
        providers = [
            {"identity": "/example/provider/FullModel",
             "roles": ["FullModel", "BackboneNeck"]},
            {"identity": "/example/provider/DetectShard0",
             "roles": ["DetectShard0"]},
            {"identity": "/example/provider/DetectShard1",
             "roles": ["DetectShard1"]},
            {"identity": "/example/provider/Merge", "roles": ["Merge"]},
        ]
    else:
        providers = [
            {"identity": f"/example/provider/{role}", "roles": [role]}
            for role in roles
        ]
    return {
        "application": "spec180-yolo",
        "controller": "/example/controller",
        "group": "/example/group",
        "services": [{
            "name": "/AI/YOLO/YOLO26n",
            "model": "/Model/YOLO26n",
            "roles": roles,
            "users": ["/example/user"],
            "providers": providers,
        }],
        "runtime": {
            "user_identity": "/example/user",
            "provider_prefix": "/example/provider",
            "nodes": {
                "controller": "memphis",
                "user": "memphis",
                "repo": "neu",
                "providers": {
                    provider["identity"]: "ucla"
                    for provider in providers
                },
            },
            "identities": {
                "controller": "/example/controller",
                "user": "/example/user",
                "repo": "/example/provider/Repo",
                "group": "/example/group",
                "providerPrefix": "/example/provider",
                "repoServicePrefix": "/NDNSF/DistributedRepo/Object",
                "providers": {
                    provider["identity"]: provider["identity"]
                    for provider in providers
                },
            },
        },
    }


def test_case_config_passes_maintained_policy_loader_checks():
    module = load_runner()
    module._validate_policy_loader_compatibility(_case_config("Y-B"))

    missing_users = _case_config("Y-B")
    missing_users["services"][0].pop("users")
    with pytest.raises(module.RunnerError,
                       match="CASE_CONFIG_POLICY_LOADER_INVALID"):
        module._validate_policy_loader_compatibility(missing_users)


def test_case_config_requires_capability_coverage_without_preassigning_roles(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    result = module._validate_case_config("Y-B", _case_config(), topology)
    assert result["providerCount"] == 4
    assert result["requiredProviderCount"] == 4
    assert result["distinctCapabilityCover"] is True
    assert result["roleCapabilities"]["Merge"] == ["/example/provider/Merge"]
    multi_capability = _case_config()
    multi_capability["services"][0]["providers"][0]["roles"] = [
        "BackboneNeck", "DetectShard0"
    ]
    updated = module._validate_case_config("Y-B", multi_capability, topology)
    assert updated["roleCapabilities"]["BackboneNeck"] == [
        "/example/provider/BackboneNeck"
    ]
    assert updated["roleCapabilities"]["DetectShard0"] == [
        "/example/provider/BackboneNeck", "/example/provider/DetectShard0"
    ]
    missing = _case_config()
    missing["services"][0]["providers"] = [
        provider for provider in missing["services"][0]["providers"]
        if provider["roles"] != ["Merge"]
    ]
    missing["services"][0]["providers"].append({
        "identity": "/example/provider/duplicate", "roles": ["DetectShard0"]})
    missing["runtime"]["nodes"]["providers"][
        "/example/provider/duplicate"] = "ucla"
    with pytest.raises(module.RunnerError, match="ROLE_CAPABILITY_MISSING"):
        module._validate_case_config("Y-B", missing, topology)


def test_case_config_requires_fixed_distinct_provider_profile(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")

    too_few = _case_config("Y-B")
    too_few["services"][0]["providers"][2]["roles"] = ["DetectShard1", "Merge"]
    too_few["services"][0]["providers"] = too_few["services"][0]["providers"][:3]
    with pytest.raises(module.RunnerError, match="PROVIDER_COUNT_INVALID"):
        module._validate_case_config("Y-B", too_few, topology)

    one_all_capable = _case_config("Y-B")
    one_all_capable["services"][0]["providers"] = [
        {"identity": "/example/provider/partial0", "roles": [
            "BackboneNeck", "DetectShard0"]},
        {"identity": "/example/provider/partial1", "roles": ["DetectShard1"]},
        {"identity": "/example/provider/partial2", "roles": ["Merge"]},
        {"identity": "/example/provider/partial3", "roles": ["Merge"]},
    ]
    with pytest.raises(module.RunnerError, match="DISTINCT_SHARED_COVERAGE_INVALID"):
        module._validate_case_config("Y-B", one_all_capable, topology)

    bad_atomic = _case_config("Y-A")
    bad_atomic["services"][0]["providers"][0]["roles"] = ["FullModel", "Merge"]
    with pytest.raises(module.RunnerError, match="PROVIDER_ROLE_UNKNOWN"):
        module._validate_case_config("Y-A", bad_atomic, topology)


def test_case_config_rejects_missing_node_mapping_or_legacy_roles(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    bad = _case_config("Y-A")
    del bad["runtime"]["nodes"]["providers"]
    with pytest.raises(module.RunnerError, match="PROVIDER_NODES_MISSING"):
        module._validate_case_config("Y-A", bad, topology)
    bad = _case_config("Y-A")
    bad["services"][0]["roles"] = ["/Stage/0"]
    with pytest.raises(module.RunnerError, match="ROLE_SET_INVALID"):
        module._validate_case_config("Y-A", bad, topology)


def _binding_inputs(tmp_path: Path, module, case: str = "Y-B"):
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    config = _case_config(case)
    case_runtime = module._validate_case_config(case, config, topology)
    policy = tmp_path / "case-policy.json"
    policy.write_text(__import__("json").dumps(config, sort_keys=True) + "\n",
                      encoding="utf-8")
    output = tmp_path / "output"
    output.mkdir()
    descriptor = {
        "schema": "spec180-yolo-case-input-v1",
        "case": case,
        "caseRuntime": case_runtime,
        "catalogueDataName": "/example/controller/NDNSF/DI/catalogue/v1",
        "catalogueSigner": "/example/controller",
        "casePolicySha256": module.digest_file(policy),
    }
    package = tmp_path / "package"
    package.mkdir()
    canonical_dir = package / "canonical"
    canonical_dir.mkdir()
    (canonical_dir / "yolo26n.onnx").write_bytes(
        b"test-only-canonical-model-placeholder")
    registry = tmp_path / "catalogue-registry.json"
    registry.write_text("{}\n", encoding="utf-8")
    trust_root = tmp_path / "offer-trust-root.json"
    trust_root.write_text("{}\n", encoding="utf-8")
    public_key_map = tmp_path / "offer-public-key-map.json"
    public_key_map.write_text("{}\n", encoding="utf-8")
    private_key_map = tmp_path / "offer-private-key-map.json"
    private_key_dir = tmp_path / "offer-private-keys"
    private_key_dir.mkdir()
    private_keys = {}
    for identity in case_runtime["providerIdentities"]:
        key_path = private_key_dir / identity.rsplit("/", 1)[-1]
        key_path.write_bytes(b"test-only-private-key-placeholder")
        private_keys[identity] = str(key_path)
    private_key_map.write_text(
        __import__("json").dumps(private_keys, sort_keys=True) + "\n",
        encoding="utf-8")
    envelope_key = tmp_path / "request-envelope.key"
    envelope_key.write_bytes(bytes(range(32)))
    envelope_key.chmod(0o600)
    descriptor["requestEnvelopeKeySha256"] = module.digest_file(envelope_key)
    return output, {
        "descriptor": descriptor,
        "case_policy": policy,
        "topology": topology,
        "package": package,
        "registry": registry,
        "offer_trust_root": trust_root,
        "offer_public_key_map": public_key_map,
        "offer_private_key_map": private_key_map,
        "envelope_key_file": envelope_key,
    }


def test_case_runtime_binding_consumes_explicit_nodes_and_identities(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    assert binding.provider_identities == (
        "/example/provider/BackboneNeck",
        "/example/provider/DetectShard0",
        "/example/provider/DetectShard1",
        "/example/provider/Merge",
    )
    adapter = module.MiniNdnCaseRuntime(binding)
    origins = adapter.route_origins()
    assert origins["memphis"][:2] == ("/example/controller", "/example/controller/DKEY")
    assert origins["ucla"][:3] == (
        "/example/provider/BackboneNeck",
        "/example/provider/BackboneNeck/KEY",
        "/example/group",
    )
    assert all(identity in origins["ucla"]
               for identity in binding.provider_identities)
    assert "/NDNSF/DistributedRepo/Object" in origins["neu"]


def test_case_runtime_binding_rejects_native_catalogue_contract_drift(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["descriptor"]["catalogueDataName"] = "/spec180/catalogue/v1"
    with pytest.raises(module.RunnerError,
                       match="CATALOG_DATA_NAME_OUTSIDE_DI_PREFIX"):
        module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)

    signer_tmp = tmp_path / "signer"
    signer_tmp.mkdir()
    output, inputs = _binding_inputs(signer_tmp, module)
    inputs["descriptor"]["catalogueSigner"] = "/example/other"
    with pytest.raises(module.RunnerError,
                       match="CATALOG_SIGNER_DATA_PREFIX_MISMATCH"):
        module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)


def test_case_runtime_binding_rejects_node_identity_mismatch_before_start(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["descriptor"]["caseRuntime"]["nodes"]["providers"][
        "/example/provider/Merge"] = "not-in-topology"
    with pytest.raises(module.RunnerError, match="NODE_NOT_IN_TOPOLOGY"):
        module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)


def test_case_runtime_binding_rejects_incomplete_node_map(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    del inputs["descriptor"]["caseRuntime"]["nodes"]["repo"]
    with pytest.raises(module.RunnerError, match="NODE_BINDING_INCOMPLETE"):
        module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)


def test_case_runtime_binding_rejects_loader_incompatible_isolated_policy(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    policy = __import__("json").loads(
        inputs["case_policy"].read_text(encoding="utf-8"))
    policy["services"][0].pop("users")
    inputs["case_policy"].write_text(
        __import__("json").dumps(policy, sort_keys=True) + "\n",
        encoding="utf-8")
    inputs["descriptor"]["casePolicySha256"] = module.digest_file(
        inputs["case_policy"])
    with pytest.raises(module.RunnerError,
                       match="CASE_CONFIG_POLICY_LOADER_INVALID"):
        module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)


def test_case_driver_rejects_missing_publication_inputs_before_network(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_PUBLICATION_INPUT_INCOMPLETE"):
        module.run_minindn_case("Y-B", output, inputs)


def test_launcher_import_path_includes_repo_client_before_network(tmp_path: Path):
    module = load_runner()
    repo_python = str(ROOT / "NDNSF-DistributedRepo/pythonWrapper")
    assert repo_python in sys.path
    # The assertion above protects the publication path, which imports
    # py_repoclient before MiniNDN is started.  The child command receives the
    # same root through its explicit PYTHONPATH in run_minindn_case().


def test_child_ndn_log_uses_dedicated_setting_without_mutating_runner_environment():
    module = load_runner()
    runner_env = {
        "NDN_LOG": "*=TRACE",
        "SPEC180_CHILD_NDN_LOG": "*=DEBUG",
        "HOME": "/home/tianxing",
        "KEEP": "value",
    }

    child_env = module._child_process_environment(runner_env)

    assert runner_env["NDN_LOG"] == "*=TRACE"
    assert runner_env["SPEC180_CHILD_NDN_LOG"] == "*=DEBUG"
    assert child_env["NDN_LOG"] == "*=DEBUG"
    assert "SPEC180_CHILD_NDN_LOG" not in child_env
    assert "HOME" not in child_env
    assert child_env["KEEP"] == "value"


def test_child_ndn_log_does_not_inherit_runner_wide_ndn_log():
    module = load_runner()
    child_env = module._child_process_environment({"NDN_LOG": "*=TRACE"})
    assert child_env["NDN_LOG"] == "*=WARN"


def test_yolo_runtime_helper_imports_current_planner_symbols():
    module_path = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"
    sys.path.insert(0, str(module_path))
    try:
        import yolo_2x2_lib
        assert yolo_2x2_lib.nxm_stage_roles(1, 2) == [
            "/Stage/0/Shard/0", "/Stage/0/Shard/1"]
    finally:
        sys.path.remove(str(module_path))


def test_case_process_specs_are_explicit_and_ack_driven(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    specs = module.MiniNdnCaseRuntime(binding, inputs).process_specs()
    assert [item.name for item in specs] == [
        "controller", "repo", "provider-BackboneNeck",
        "provider-DetectShard0", "provider-DetectShard1", "provider-Merge",
        "user",
    ]
    assert [item.node for item in specs[:2]] == ["memphis", "neu"]
    assert specs[0].ready_marker == "SPEC180_CONTROLLER_READY"
    assert all("provider_role_assignments" not in item.command
               for item in specs)
    assert all("--config" in item.command and "case-policy.json" in item.command
               for item in specs if not item.name.startswith("provider-"))
    user = specs[-1]
    assert "--ack-timeout-ms 1500" in user.command
    assert "--canonical-package" in user.command
    assert "--lifecycle-output-dir" in user.command
    assert "--lifecycle-case Y-B" in user.command
    assert "--request-id /spec180-y-b-" in user.command
    assert "--envelope-key-file" in user.command
    assert str(inputs["envelope_key_file"]) in user.command
    assert user.ready_marker == "YOLO_ACK_DRIVEN_RESULT"
    provider_commands = [item.command for item in specs if item.name.startswith("provider-")]
    assert all("--selection-offer-key-file" in command
               for command in provider_commands)
    assert all("di-native-provider" in command
               for command in provider_commands)
    assert all("provider.py" not in command
               for command in provider_commands)
    assert all("--serve" in command
               and "--plan" in command
               and "native-execution-plan.json" in command
               and "--manifest" in command
               and "service-manifest.json" in command
               and "--trust-schema" in command
               and "--offer-backend onnxruntime-cpu" in command
               and "--offer-can-provision" in command
               for command in provider_commands)
    assert all(item.ready_marker == "NDNSF_DI_NATIVE_PROVIDER_READY"
               for item in specs if item.name.startswith("provider-"))
    assert "--native-tensor-input" in user.command


def test_y_n_c_mutates_native_provider_capabilities_at_process_boundary(
        tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module, "Y-N")
    inputs["subcase"] = "Y-N-C"
    binding = module.CaseRuntimeBinding.from_inputs("Y-N", output, inputs)

    specs = module.MiniNdnCaseRuntime(binding, inputs).process_specs("providers")
    commands = {item.name: item.command for item in specs}

    assert "--roles BackboneNeck" in commands["provider-FullModel"]
    assert "--roles DetectShard0" in commands["provider-DetectShard0"]
    assert "--roles DetectShard1" in commands["provider-DetectShard1"]
    assert "--roles BackboneNeck" in commands["provider-Merge"]
    assert "--roles FullModel,BackboneNeck" not in commands["provider-FullModel"]
    assert "--roles Merge" not in commands["provider-Merge"]


def test_request_envelope_key_must_be_owner_only_and_exactly_32_bytes(
        tmp_path: Path):
    module = load_runner()
    key = tmp_path / "request-envelope.key"
    key.write_bytes(b"k" * 31)
    key.chmod(0o600)
    with pytest.raises(module.RunnerError,
                       match="REQUEST_ENVELOPE_KEY_SIZE_INVALID"):
        module._validate_envelope_key_file(str(key))

    key.write_bytes(b"k" * 32)
    key.chmod(0o640)
    with pytest.raises(module.RunnerError,
                       match="REQUEST_ENVELOPE_KEY_PERMISSIONS_INVALID"):
        module._validate_envelope_key_file(str(key))

    key.chmod(0o600)
    assert module._validate_envelope_key_file(str(key)) == key.resolve()


def test_runtime_publication_is_the_controller_barrier_before_repo_start(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    publication = tmp_path / "runtime-publication.json"
    publication.write_text("{}\n", encoding="utf-8")
    inputs["runtime_publication_file"] = publication
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)

    controller = module.MiniNdnCaseRuntime(binding, inputs).process_specs("control")[0]

    assert controller.ready_marker == "SPEC180_RUNTIME_CATALOGUE_PUBLISHED"


def test_case_process_specs_use_barriered_startup_phases(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    specs = runtime.process_specs()
    assert [item.startup_phase for item in specs] == [
        "control", "control", "providers", "providers", "providers",
        "providers", "user",
    ]
    assert [item.name for item in runtime.process_specs("control")] == [
        "controller", "repo",
    ]
    assert len(runtime.process_specs("providers")) == 4
    assert [item.name for item in runtime.process_specs("user")] == ["user"]
    with pytest.raises(module.RunnerError, match="START_PHASE_INVALID"):
        runtime.process_specs("all")


def test_control_start_waits_for_controller_before_repository(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module, "Y-B")
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)

    class Proc:
        def poll(self):
            return None

    class Node:
        def __init__(self, name):
            self.name = name
            self.params = {"params": {"homeDir": str(tmp_path / name)}}

    class Net:
        def __getitem__(self, name):
            return Node(name)

    class Ndn:
        net = Net()

    class Legacy:
        PY_DIR = str(tmp_path)
        REPO = str(tmp_path)

        @staticmethod
        def python_cmd(script, argv, *, repo, py_dir):
            return "python3 " + script

        def __init__(self):
            self.started = []
            self.envs = {}

        def start(self, node, name, command, env, procs, **kwargs):
            path = output / (name + ".log")
            path.write_text(
                "ServiceController listening\nSPEC180_CONTROLLER_READY\n"
                if name == "controller" else "")
            proc = Proc()
            self.started.append(name)
            self.envs[name] = dict(env)
            handle = path.open("ab")
            procs.append((proc, handle, path))
            return proc, path

        @staticmethod
        def stop_process_group(procs):
            for _proc, handle, _path in procs:
                handle.close()

    legacy = Legacy()
    runtime._legacy = legacy
    started = runtime.start_processes(Ndn(), {}, [], phase="control")
    assert legacy.started == ["controller", "repo"]
    assert [item[0].name for item in started] == ["controller", "repo"]
    assert legacy.envs["controller"]["NDN_CLIENT_PIB"].endswith(
        "/memphis/.ndn")
    assert legacy.envs["controller"]["NDN_CLIENT_TPM"].endswith(
        "/memphis/.ndn")


def test_initialize_keychains_rewrites_node_tpm_locator(tmp_path: Path):
    module = load_runner()
    node_home = tmp_path / "memphis"
    (node_home / ".ndn").mkdir(parents=True)
    pib_path = node_home / ".ndn" / "pib.db"
    with sqlite3.connect(pib_path) as connection:
        connection.execute("CREATE TABLE tpmInfo(tpm_locator BLOB)")
        connection.execute("INSERT INTO tpmInfo VALUES (?)", ("tpm-file:",))

    module._rewrite_node_tpm_locator(node_home, "memphis")

    with sqlite3.connect(pib_path) as connection:
        assert connection.execute("SELECT tpm_locator FROM tpmInfo").fetchone()[0] == (
            "tpm-file:" + str(node_home / ".ndn"))


def test_start_processes_rejects_invalid_phase_before_side_effects(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    with pytest.raises(module.RunnerError, match="START_PHASE_INVALID"):
        runtime.start_processes(object(), {}, [], phase="all")


def test_start_processes_rejects_duplicate_provider_phase_before_side_effects(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    runtime._started_phases.update({"control", "providers"})
    # No network or process-launch method exists on this sentinel. The existing
    # phase guard must reject before accessing either of them.
    with pytest.raises(module.RunnerError, match="START_PHASE_DUPLICATE:providers"):
        runtime.start_processes(object(), {}, [], phase="providers")


def test_start_processes_enforces_phase_preconditions_and_catalogue_barrier(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    with pytest.raises(module.RunnerError, match="START_PHASE_PRECONDITION:control"):
        runtime.start_processes(object(), {}, [], phase="providers")
    with pytest.raises(module.RunnerError, match="CATALOGUE_PUBLICATION_PHASE_INVALID"):
        runtime.mark_catalogue_published(
            data_name="/example/controller/NDNSF/DI/catalogue/v1",
            signer="/example/controller",
            data_digest="sha256:" + "a" * 64,
        )
    runtime._started_phases.update({"control", "providers"})
    with pytest.raises(module.RunnerError, match="READY_PHASE_PRECONDITION:control"):
        runtime.start_processes(object(), {}, [], phase="user")
    runtime._ready_phases.update({"control", "providers"})
    with pytest.raises(module.RunnerError, match="CATALOGUE_PUBLICATION_REQUIRED"):
        runtime.start_processes(object(), {}, [], phase="user")


def test_start_processes_rolls_back_partial_phase_launch(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)

    class Proc:
        def __init__(self):
            self.signaled = False
            self.waited = False

        def poll(self):
            return None if not self.signaled else 0

        def send_signal(self, _signal):
            self.signaled = True

        def wait(self, timeout=None):
            self.waited = True

    class Net:
        def __getitem__(self, name):
            return type("Node", (), {
                "name": name,
                "params": {"params": {"homeDir": str(tmp_path / name)}},
            })()

    class Ndn:
        net = Net()

    class Legacy:
        def __init__(self):
            self.calls = 0
            self.started = []
            self.PY_DIR = str(tmp_path)
            self.REPO = str(tmp_path)

        @staticmethod
        def python_cmd(script, argv, *, repo, py_dir):
            return "python3 " + script + " " + " ".join(argv)

        def start(self, node, name, command, env, procs, **kwargs):
            self.calls += 1
            if self.calls == 2:
                raise OSError("simulated launch failure")
            proc = Proc()
            log = output / (name + ".log")
            log.write_text(
                ("ServiceController listening\nSPEC180_CONTROLLER_READY\n"
                 if name == "controller" else ""),
                encoding="utf-8",
            )
            file_handle = log.open("ab")
            procs.append((proc, file_handle, log))
            self.started.append(proc)
            return proc, log

        def stop_process_group(self, procs):
            for proc, file_handle, _log in reversed(procs):
                proc.send_signal(2)
                proc.wait(timeout=3)
                file_handle.close()

    legacy = Legacy()
    runtime._legacy = legacy
    procs = []
    with pytest.raises(module.RunnerError,
                       match="PROCESS_START_FAILED:control"):
        runtime.start_processes(Ndn(), {}, procs, phase="control")
    assert procs == []
    assert legacy.started[0].signaled is True
    assert legacy.started[0].waited is True
    assert runtime._started_phases == set()
    failure_text = (output / "process-start-failure.json").read_text()
    failure = json.loads(failure_text)
    assert failure["phase"] == "control"
    assert failure["errorType"] == "OSError"
    assert failure["frames"][-1]["function"] == "start"
    assert failure["frames"][-1]["line"] > 0
    assert "simulated launch failure" not in failure_text
    assert "locals" not in failure_text


def test_catalogue_publication_receipt_is_name_signer_and_digest_bound(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    runtime._started_phases.update({"control", "providers"})
    runtime._ready_phases.update({"control", "providers"})
    with pytest.raises(module.RunnerError, match="CATALOGUE_NAME_MISMATCH"):
        runtime.mark_catalogue_published(
            data_name="/wrong/catalogue",
            signer="/example/controller",
            data_digest="sha256:" + "a" * 64,
        )
    runtime.mark_catalogue_published(
        data_name="/example/controller/NDNSF/DI/catalogue/v1",
        signer="/example/controller",
        data_digest="sha256:" + "a" * 64,
    )
    with pytest.raises(module.RunnerError, match="CATALOGUE_PUBLICATION_DUPLICATE"):
        runtime.mark_catalogue_published(
            data_name="/example/controller/NDNSF/DI/catalogue/v1",
            signer="/example/controller",
            data_digest="sha256:" + "b" * 64,
        )


def test_runtime_catalogue_publisher_requires_signed_exact_readback(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["manifest"] = _case_manifest()
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    runtime._started_phases.update({"control", "providers"})
    runtime._ready_phases.update({"control", "providers"})

    digest = "sha256:" + "b" * 64
    snapshot = PreSplitCatalogSnapshot(
        alias="yolo-shared",
        manifest_digest=digest,
        model_content_digest=digest,
        semantics_digest=digest,
        graph_digest=digest,
        candidate_digest=digest,
        backend="onnxruntime-cpu",
        precision="float32",
        artifact_data_names={
            role: ("/repo/segments/" + role.lower(),)
            for role in ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
        },
        status="ACTIVE",
        created_at_ms=1,
    )

    class Publisher:
        def __init__(self):
            self.published = None
            self.fetched = None

        def publish_signed_app_data(self, name, payload, *, freshness_ms):
            self.published = (name, bytes(payload), freshness_ms)
            return type("Publish", (), {
                "success": True,
                "data_name": name,
                "error": "",
            })()

        def fetch_signed_app_data(self, name, signer, *, timeout_ms):
            assert signer == "/example/controller"
            assert timeout_ms == 5000
            return type("Fetch", (), {
                "success": True,
                "data_name": name,
                "payload": self.published[1],
                "signer_certificate": "/example/controller/KEY/ksk-test",
                "error": "",
            })()

    publisher = Publisher()
    receipt = runtime.publish_and_verify_runtime_catalogue(
        publisher, [snapshot])
    assert receipt["dataName"] == "/example/controller/NDNSF/DI/catalogue/v1"
    assert receipt["signer"] == "/example/controller"
    assert receipt["payloadDigest"].startswith("sha256:")
    with pytest.raises(module.RunnerError, match="PUBLICATION_DUPLICATE"):
        runtime.publish_and_verify_runtime_catalogue(publisher, [snapshot])

    tampered_root = tmp_path / "tampered"
    tampered_root.mkdir()
    output2, inputs2 = _binding_inputs(tampered_root, module)
    inputs2["manifest"] = _case_manifest()
    binding2 = module.CaseRuntimeBinding.from_inputs("Y-B", output2, inputs2)
    runtime2 = module.MiniNdnCaseRuntime(binding2, inputs2)
    runtime2._started_phases.update({"control", "providers"})
    runtime2._ready_phases.update({"control", "providers"})

    class Tampered(Publisher):
        def fetch_signed_app_data(self, name, signer, *, timeout_ms):
            result = super().fetch_signed_app_data(
                name, signer, timeout_ms=timeout_ms)
            result.payload = bytes(result.payload) + b"tampered"
            return result

    with pytest.raises(module.RunnerError, match="READBACK_DIGEST_MISMATCH"):
        runtime2.publish_and_verify_runtime_catalogue(Tampered(), [snapshot])

    missing_signer_root = tmp_path / "missing-signer"
    missing_signer_root.mkdir()
    output3, inputs3 = _binding_inputs(missing_signer_root, module)
    inputs3["manifest"] = _case_manifest()
    binding3 = module.CaseRuntimeBinding.from_inputs("Y-B", output3, inputs3)
    runtime3 = module.MiniNdnCaseRuntime(binding3, inputs3)
    runtime3._started_phases.update({"control", "providers"})
    runtime3._ready_phases.update({"control", "providers"})

    class MissingSigner(Publisher):
        def fetch_signed_app_data(self, name, signer, *, timeout_ms):
            result = super().fetch_signed_app_data(
                name, signer, timeout_ms=timeout_ms)
            result.signer_certificate = ""
            return result

    with pytest.raises(module.RunnerError,
                       match="READBACK_SIGNER_MISMATCH"):
        runtime3.publish_and_verify_runtime_catalogue(
            MissingSigner(), [snapshot])


def test_process_specs_require_files_for_non_package_inputs(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["registry"].unlink()
    inputs["registry"].mkdir()
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    with pytest.raises(module.RunnerError, match="CASE_PROCESS_INPUT_INVALID:registry"):
        module.MiniNdnCaseRuntime(binding, inputs).process_specs()


def test_process_specs_require_candidate_bound_private_offer_key_map(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs.pop("offer_private_key_map")
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    with pytest.raises(module.RunnerError,
                       match="CASE_PROCESS_INPUTS_INCOMPLETE:offer_private_key_map"):
        module.MiniNdnCaseRuntime(binding, inputs).process_specs()


def test_process_specs_rechecks_declared_private_key_digests(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    private_key_map = inputs["offer_private_key_map"]
    private_keys = __import__("json").loads(
        private_key_map.read_text(encoding="utf-8"))
    descriptor = inputs["descriptor"]
    descriptor["offerPrivateKeyMapSha256"] = module.digest_file(private_key_map)
    descriptor["offerPrivateKeysSha256"] = {
        identity: module.digest_file(Path(path))
        for identity, path in private_keys.items()
    }
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    runtime.process_specs()

    first_key = Path(next(iter(private_keys.values())))
    first_key.write_bytes(b"mutated-private-key")
    with pytest.raises(module.RunnerError,
                       match="CASE_PROCESS_OFFER_PRIVATE_KEY_DIGEST_MISMATCH"):
        runtime.process_specs()


def test_wait_for_ready_requires_markers_and_rejects_early_exit(tmp_path: Path):
    module = load_runner()

    class Proc:
        def __init__(self, returncode=None):
            self.returncode = returncode

        def poll(self):
            return self.returncode

    log = tmp_path / "provider.log"
    log.write_text("provider started\nREADY\n", encoding="utf-8")
    spec = module.CaseProcessSpec("provider", "node", "cmd", "READY", "providers")
    module.wait_for_ready(((spec, Proc(), log),), timeout_s=0.1)

    exited_log = tmp_path / "exited.log"
    exited_spec = module.CaseProcessSpec("exited", "node", "cmd", "READY", "providers")
    with pytest.raises(module.RunnerError, match="EXITED_BEFORE_READY:exited"):
        module.wait_for_ready(((exited_spec, Proc(3), exited_log),), timeout_s=0.1)


def test_runtime_wait_for_ready_closes_the_phase_state(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    runtime._started_phases.add("providers")
    log = tmp_path / "ready.log"
    log.write_text("Installed provider permission\n", encoding="utf-8")

    class Proc:
        def poll(self):
            return None

    spec = module.CaseProcessSpec(
        "provider", "node", "cmd", "Installed provider permission", "providers")
    runtime.wait_for_ready(((spec, Proc(), log),), timeout_s=0.1)
    assert runtime._ready_phases == {"providers"}


def test_start_network_failure_stops_partial_network_and_normalizes_error(
        tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    calls = []

    class Network:
        def __init__(self, **_kwargs):
            self.net = SimpleNamespace(hosts=[])

        def start(self):
            calls.append("network.start")

        def stop(self):
            calls.append("network.stop")

    class Minindn(Network):
        @staticmethod
        def cleanUp():
            calls.append("minindn.cleanUp")

        @staticmethod
        def verifyDependencies():
            calls.append("minindn.verifyDependencies")

    def fail_wait(_ndn, _output):
        raise RuntimeError("nfd not ready")

    runtime._legacy = SimpleNamespace(
        Minindn=Minindn,
        AppManager=lambda *_args, **_kwargs: None,
        Nfd=object(),
        perf=SimpleNamespace(wait_for_nfd_sockets=fail_wait),
    )

    with pytest.raises(module.RunnerError,
                       match="CASE_RUNTIME_NETWORK_START_FAILED"):
        runtime.start_network()

    assert calls == [
        "minindn.cleanUp", "minindn.verifyDependencies", "network.start",
        "network.stop", "minindn.cleanUp",
    ]
    assert runtime._ndn is None


def test_runtime_stop_is_owned_idempotent_and_cleans_network(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    calls: list[str] = []

    class Network:
        def stop(self):
            calls.append("network.stop")

    class Minindn:
        @staticmethod
        def cleanUp():
            calls.append("minindn.cleanUp")

    def stop_process_group(entries):
        calls.append("processes:" + str(len(entries)))

    runtime._legacy = SimpleNamespace(
        Minindn=Minindn,
        stop_process_group=stop_process_group,
    )
    runtime._ndn = Network()
    handle = SimpleNamespace()
    runtime._processes.append((handle, handle, tmp_path / "child.log"))
    runtime._started_phases.update({"control", "providers", "user"})
    runtime._ready_phases.update({"control", "providers", "user"})
    runtime._catalogue_publication_digest = "sha256:" + "a" * 64

    runtime.stop()
    runtime.stop()

    assert calls == ["processes:1", "network.stop", "minindn.cleanUp"]
    assert runtime._processes == []
    assert runtime._ndn is None
    assert runtime._started_phases == set()
    assert runtime._ready_phases == set()
    assert runtime._catalogue_publication_digest is None
    with pytest.raises(module.RunnerError, match="ALREADY_STOPPED"):
        runtime.start_network()


def test_runtime_stop_uses_legacy_process_handles_after_start(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module, "Y-B")
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    stopped = []

    class Proc:
        def poll(self):
            return None

    class Node:
        def __init__(self):
            self.params = {"params": {"homeDir": str(tmp_path / "memphis")}}

    class Net:
        def __getitem__(self, _name):
            return Node()

    class Ndn:
        net = Net()

    class Minindn:
        @staticmethod
        def cleanUp():
            pass

    class Legacy:
        PY_DIR = str(tmp_path)
        REPO = str(tmp_path)

        @staticmethod
        def python_cmd(script, argv, *, repo, py_dir):
            return "python3 " + script

        @staticmethod
        def start(_node, name, _command, _env, procs, **_kwargs):
            log = output / (name + ".log")
            log.write_text(
                ("ServiceController listening\nSPEC180_CONTROLLER_READY\n"
                 if name == "controller" else ""))
            handle = log.open("ab")
            proc = Proc()
            procs.append((proc, handle, log))
            return proc, log

        @staticmethod
        def stop_process_group(entries):
            stopped.extend(entries)
            for proc, handle, _path in entries:
                assert hasattr(proc, "poll")
                handle.close()

    Legacy.Minindn = Minindn
    runtime._legacy = Legacy()
    legacy_processes = []
    runtime.start_processes(Ndn(), {}, legacy_processes, phase="control")
    runtime.stop()

    assert stopped == legacy_processes


def test_case_process_specs_reject_repo_identity_not_in_provider_namespace(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    inputs["descriptor"]["caseRuntime"]["identities"]["repo"] = "/example/repo"
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    with pytest.raises(module.RunnerError,
                       match="CASE_PROCESS_REPO_IDENTITY_OUTSIDE_PROVIDER_PREFIX"):
        module.MiniNdnCaseRuntime(binding, inputs).process_specs()


def test_case_config_requires_explicit_runtime_identity_set(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    config = _case_config("Y-A")
    del config["runtime"]["identities"]
    with pytest.raises(module.RunnerError, match="IDENTITIES_MISSING"):
        module._validate_case_config("Y-A", config, topology)


def test_case_config_rejects_duplicate_provider_identity(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    config = _case_config("Y-A")
    providers = config["services"][0]["providers"]
    providers.append(dict(providers[0]))
    with pytest.raises(module.RunnerError, match="CASE_CONFIG_PROVIDER_DUPLICATE"):
        module._validate_case_config("Y-A", config, topology)


def test_case_config_rejects_provider_namespace_drift(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    config = _case_config("Y-A")
    config["runtime"]["identities"]["providerPrefix"] = "/other/provider"
    with pytest.raises(module.RunnerError, match="PROVIDER_PREFIX_MISMATCH"):
        module._validate_case_config("Y-A", config, topology)


def test_case_config_rejects_missing_provider_prefix_as_runner_error(tmp_path: Path):
    module = load_runner()
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\narizona:\nwustl:\nneu:\n")
    config = _case_config("Y-A")
    del config["runtime"]["identities"]["providerPrefix"]
    with pytest.raises(module.RunnerError, match="IDENTITY_INVALID:providerPrefix"):
        module._validate_case_config("Y-A", config, topology)


def test_initialize_keychains_uses_case_identity_parent(tmp_path: Path):
    module = load_runner()
    output, inputs = _binding_inputs(tmp_path, module)
    binding = module.CaseRuntimeBinding.from_inputs("Y-B", output, inputs)
    runtime = module.MiniNdnCaseRuntime(binding, inputs)
    calls = []

    class Legacy:
        def initialize_di_keychains(self, *args, **kwargs):
            calls.append((args, kwargs))

    runtime._legacy = Legacy()
    # The production method also rewrites node-scoped TPM locators.  Supply
    # the smallest network-shaped stub so this unit stays focused on the
    # legacy keychain argument contract without triggering real filesystem IO.
    runtime.initialize_keychains(SimpleNamespace(net=SimpleNamespace(hosts=[])))
    assert len(calls) == 1
    _args, kwargs = calls[0]
    assert kwargs["app_root"] == "/example"
    assert kwargs["controller_identity"] == "/example/controller"
    assert kwargs["user_identity"] == "/example/user"


def test_case_policy_isolated_copy_filters_unrelated_roles(tmp_path: Path):
    module = load_runner()
    config = _case_config("Y-N")
    config["services"][0]["providers"][0]["roles"] = [
        "FullModel", "BackboneNeck"
    ]
    config["services"].append({
        "name": "/NDNSF/DistributedRepo/Object/v1/FETCH",
        "roles": [],
        "providers": [],
    })
    output = tmp_path / "case-output"
    output.mkdir()
    overlap_policy = module._materialize_case_config("Y-N", config, output)
    overlap_written = __import__("json").loads(
        overlap_policy.read_text())
    overlap_inference = [item for item in overlap_written["services"]
                         if item["name"] == "/AI/YOLO/YOLO26n"][0]
    overlap_by_identity = {
        item["identity"]: item["roles"]
        for item in overlap_inference["providers"]
    }
    assert overlap_by_identity["/example/provider/FullModel"] == [
        "FullModel", "BackboneNeck"
    ]
    policy = module._materialize_case_config("Y-A", config, output)
    assert policy.name == "case-policy.json"
    written = __import__("json").loads(policy.read_text())
    inference = [item for item in written["services"]
                 if item["name"] == "/AI/YOLO/YOLO26n"][0]
    assert inference["roles"] == ["FullModel"]
    assert [item["roles"] for item in inference["providers"]] == [["FullModel"]]
    assert config["services"][0]["roles"] == [
        "FullModel", "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"
    ]


def test_case_policy_materializes_complete_repo_authorization(tmp_path: Path):
    module = load_runner()
    config = _case_config("Y-A")
    output = tmp_path / "output"
    output.mkdir()

    policy = module._materialize_case_config("Y-A", config, output)
    document = __import__("json").loads(policy.read_text(encoding="utf-8"))
    from py_repoclient.service_names import repo_versioned_services

    repo_services = {
        item["name"]: item for item in document["services"]
        if item["name"].startswith("/NDNSF/DistributedRepo/")
    }
    assert set(repo_services) == set(repo_versioned_services())
    expected_clients = {
        "/example/controller", "/example/user", "/example/provider/Repo",
        "/example/provider/FullModel",
    }
    for service in repo_services.values():
        assert set(service["users"]) == expected_clients
        assert service["providers"] == [{
            "identity": "/example/provider/Repo", "roles": []}]


def test_exact_sif_command_provider_contract_is_present():
    """The replay driver requires the SIF command-provider contract."""
    source = SCRIPT.read_text(encoding="utf-8")
    for marker in ("SPEC180_RUNTIME_SIF", "SPEC180_RUNTIME_APPTAINER",
                   "sif_exec_prefix", "--cleanenv",
                   "SPEC180_SIF_HOST_PROCESS_FALLBACK"):
        assert marker in source, f"missing SIF contract marker: {marker}"
    assert "Spec180SifNfd" in source


def test_exact_sif_prefix_only_when_runtime_declared(tmp_path: Path,
                                                     monkeypatch):
    """Without the declared image the runner stays a pure host process."""
    monkeypatch.delenv("SPEC180_RUNTIME_SIF", raising=False)
    module = load_runner()
    assert module.sif_runtime_enabled() is False
    assert module.sif_exec_prefix({}) == ""
    assert module.sif_python_prefix({}) == ""


def test_exact_sif_prefix_binds_per_node_home_and_sealed_pwd(
        tmp_path: Path, monkeypatch):
    monkeypatch.setenv("SPEC180_RUNTIME_SIF", "/opt/sifs/spec180.sif")
    monkeypatch.setenv("SPEC180_RUNTIME_APPTAINER",
                       "/opt/apptainer/1.5.3/bin/apptainer")
    module = load_runner()
    assert module.sif_runtime_enabled() is True
    prefix = module.sif_exec_prefix({}, home_dir=str(tmp_path / "node-home"))
    assert "--cleanenv" in prefix
    assert f"--home {tmp_path / 'node-home'}:{tmp_path / 'node-home'}" in prefix
    assert "--pwd /opt/ndnsf-di/replay/repo" in prefix
    assert "PYTHONNOUSERSITE=1" in prefix
    assert "/opt/sifs/spec180.sif" in prefix
    # Secret-bearing NDNSF env values pass through; launcher identities do not.
    assert "--env NDNSF_DI_STATE_ROOT=/tmp/state" in module.sif_exec_prefix(
        {"NDNSF_DI_STATE_ROOT": "/tmp/state",
         "SPEC180_RUNTIME_SIF": "/opt/sifs/spec180.sif"})


def test_exact_sif_prefix_preserves_node_keychain_locators(
        tmp_path: Path, monkeypatch):
    """SIF cleanenv must retain the node PIB/TPM pair used by Runtime::open."""
    monkeypatch.setenv("SPEC180_RUNTIME_SIF", "/opt/sifs/spec180.sif")
    monkeypatch.setenv("SPEC180_RUNTIME_APPTAINER",
                       "/opt/apptainer/1.5.3/bin/apptainer")
    module = load_runner()
    node_home = tmp_path / "node-home"
    prefix = module.sif_exec_prefix(
        {"NDN_CLIENT_PIB": f"pib-sqlite3:{node_home}/.ndn",
         "NDN_CLIENT_TPM": f"tpm-file:{node_home}/.ndn"},
        home_dir=str(node_home))
    assert f"--env NDN_CLIENT_PIB=pib-sqlite3:{node_home}/.ndn" in prefix
    assert f"--env NDN_CLIENT_TPM=tpm-file:{node_home}/.ndn" in prefix
