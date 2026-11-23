"""Offline checks for the MiniNDN withdrawal evidence reader."""

import json

import pytest

from withdrawal import in_flight, verify


REQUEST = "/test/req/2"
TARGET = "/test/p0"
ROLE = "/stage/rank/0"
PLAN = "sha256:" + "a" * 64
BINDING = dict(requestId=REQUEST, attemptEpoch="2", provider=TARGET, role=ROLE, planDigest=PLAN)
FIELDS = " ".join(k + "=" + v for k, v in BINDING.items())
STAMP = "1000000000.800000 "


def test_trigger_requires_execution_of_selected_second_turn():
    user = ["NATIVE_CONVERSATION_TURN_START index=1 request=" + REQUEST]
    target = [STAMP + "NDNSF_DI_NATIVE_SELECTION_ACCEPTED " + FIELDS]
    started = STAMP + "NDNSF_DI_PROVIDER_STAGE stage=EXECUTION_ENTERED status=observed " + FIELDS
    assert in_flight(user, target, TARGET, ROLE) is None
    assert in_flight(user, target + [started.replace("attemptEpoch=2", "attemptEpoch=1")],
                     TARGET, ROLE) is None
    assert in_flight(user, target + [started], TARGET, ROLE)[0] == BINDING
    with pytest.raises(RuntimeError, match="already terminated"):
        in_flight(user, target + [started, started.replace("EXECUTION_ENTERED", "TERMINAL")],
                  TARGET, ROLE)


@pytest.mark.parametrize("failure", ["", "missing_exit", "signal", "wrong_attempt", "early_terminal",
                                      "late_checkpoint", "untargeted_controller", "lease_only"])
def test_result_binds_target_attempt_and_requester_wait_status(tmp_path, failure):
    actors = dict(requester="user", target="p0", controller="controller")
    ids = ["user", "p0", "p1", "p2", "repo", "authority", "controller"]
    cfg = {"network": {"service": "/test/infer"}, "actors": [{"id": name} for name in ids]}
    rows = [{"id": name, "status": "ZERO", "forced": False, "exitCode": 1 if name == "user" else 0}
            for name in ids]
    if failure == "missing_exit":
        rows[0]["exitCode"] = None
    if failure == "signal":
        rows[0]["exitCode"] = -11
    (tmp_path / "operation.json").write_text(json.dumps({"status": "STOPPED", "failure": "REQUESTER_EXIT:1",
        "requester": {"exit_code": 1}, "cleanup": rows}))
    (tmp_path / "controller.log").write_text(STAMP + "NDNSF_CONTROLLER_REVOKED kind=3 identity=" +
        ("/test/p2" if failure == "untargeted_controller" else TARGET) + " service=/test/infer generation=3 epoch=2\n" +
        "NDNSF_REVOCATION_APPLIED success=1 kind=3 generation=3 epoch=2\n")
    (tmp_path / "p0.log").write_text(STAMP + "NDNSF_PROVIDER_AUTHORITY_WITHDRAWN lost=1 epoch=2\n" +
        STAMP + "NDNSF_DI_PROVIDER_STAGE stage=TERMINAL status=failed " + FIELDS + "\n")
    err = {"requestId": REQUEST, "attemptEpoch": 1 if failure == "wrong_attempt" else 2,
           "code": "NATIVE_STREAM_FAILED", "boundary": "stream",
           "epochMs": 1000000000000 + (100 if failure == "early_terminal" else 800),
           "message": "Core stream failed: stream failed because the Controller authority advanced for /test/infer"}
    if failure == "lease_only":
        err["message"] = "Core stream failed: Controller policy lease expired for /test/infer"
    (tmp_path / "user.log").write_text("NDNSF_DI_REQUEST_TERMINAL " + json.dumps(err) + "\n")
    second = {"output_file": str(tmp_path / "out"), "checkpoint_output_file": str(tmp_path / "chk")}
    if failure == "late_checkpoint":
        (tmp_path / "chk").touch()
    if failure:
        with pytest.raises(RuntimeError):
            verify(cfg, tmp_path, actors, BINDING, 1000000000500000000, second)
    else:
        assert verify(cfg, tmp_path, actors, BINDING, 1000000000500000000, second)["epoch"] == "2"
