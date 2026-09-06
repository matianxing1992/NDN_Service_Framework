"""Execute the maintained request exception handler, without starting NFD."""
from __future__ import annotations

import ast
import __future__
import json
from pathlib import Path
import re
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
USER = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py"


def request_failure(case, error, *, last_milestone="GRAPH_READY"):
    tree = ast.parse(USER.read_text())
    request_try = next(
        node for node in ast.walk(tree) if isinstance(node, ast.Try)
        and any(isinstance(call, ast.Call)
                and isinstance(call.func, ast.Attribute)
                and call.func.attr == "request_task"
                for statement in node.body for call in ast.walk(statement)))
    definitions = [node for node in tree.body if
                   (isinstance(node, ast.FunctionDef)
                    and (node.name.startswith("_spec180_negative")
                         or node.name == "_emit_spec180_y_n_negative"))
                   or (isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id.startswith("_YN_")
                               for t in node.targets))]
    wrapper = ast.parse("def run():\n    pass\n").body[0]
    wrapper.body = [request_try]

    def fail(**kwargs):
        raise error

    args = SimpleNamespace(timeout_ms=10, request_id="/spec180-regression",
                           lifecycle_case=case)
    namespace = dict(
        mutation=case, args=args, re=re, json=json, Path=Path,
        RunnerError=type("RunnerError", (RuntimeError,), {}),
        client=SimpleNamespace(request_task=fail), model=None, task=None,
        app_input=None, adapter=SimpleNamespace(
            descriptor=SimpleNamespace(options_schema_digest="fixture")),
        TaskOptions=lambda *args: None,
        journal=SimpleNamespace(request_id=args.request_id, attempt_id="attempt-1",
                                last_milestone=last_milestone))
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=definitions + [wrapper], type_ignores=[])), str(USER), "exec",
        flags=__future__.annotations.compiler_flag), namespace)
    return namespace["run"]()


@pytest.mark.parametrize("case", ["Y-N-C", "Y-N-P", "Y-N-R", "Y-N-E"])
@pytest.mark.parametrize("error", [RuntimeError("UNRELATED_REPOSITORY_TIMEOUT"),
                                  ValueError("unrelated malformed graph")])
def test_unrelated_request_error_never_becomes_negative_pass(case, error, capsys):
    with pytest.raises(type(error), match=re.escape(str(error))):
        request_failure(case, error)
    assert "status=PASS" not in capsys.readouterr().out


@pytest.mark.parametrize("case,error,phase", [
    ("Y-N-P", ValueError("Provider ACK failed Trust Schema verification"), "GRAPH_READY"),
    ("Y-N-R", ValueError("range/rank role requires a non-empty layer interval"),
     "PLACEMENT_DECISION"),
])
def test_expected_error_requires_observed_phase(case, error, phase, capsys):
    assert request_failure(case, error, last_milestone=phase) == 91
    assert "status=PASS" in capsys.readouterr().out
    with pytest.raises(ValueError):
        request_failure(case, error, last_milestone="REQUEST_SENT")
    assert "status=PASS" not in capsys.readouterr().out


def test_strategy_internal_error_disguised_as_infeasible_is_not_negative_pass(capsys):
    error = ValueError("V3 strategy found no feasible graph candidate "
                       "(sha256:" + "a" * 64 + ":RuntimeError:internal failure)")
    with pytest.raises(ValueError):
        request_failure("Y-N-C", error)
    assert "status=PASS" not in capsys.readouterr().out


def test_fabricated_epoch_error_is_not_accepted(capsys):
    with pytest.raises(RuntimeError):
        request_failure("Y-N-E", RuntimeError("DI_PROTECTION_EPOCH_REJECTED"),
                        last_milestone="ARTIFACTS_READY")
    assert "status=PASS" not in capsys.readouterr().out


def test_actual_distinct_provider_infeasibility_is_accepted(capsys):
    detail = "sha256:" + "a" * 64 + ":ValueError:no distinct feasible Provider for V3 role Merge#0"
    error = ValueError("V3 strategy found no feasible graph candidate (" + detail + ")")
    assert request_failure("Y-N-C", error) == 91
    assert "requestId=/spec180-regression" in capsys.readouterr().out
