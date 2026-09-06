"""Exercise the real V3 sealing and commit expressions across their contract."""
import ast
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from ndnsf import CollaborationRole
from ndnsf_distributed_inference.core.contracts import DATA_DRIVEN_V2
from ndnsf_distributed_inference.plan import SealedCollaborationPlan

ROOT = Path(__file__).resolve().parents[2]
DIGEST = "sha256:" + "a" * 64


def seal_and_commit(fetches):
    source = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py"
    tree = ast.parse(source.read_text())
    function = next(n for n in ast.walk(tree)
                    if isinstance(n, ast.FunctionDef) and n.name == "_request_v3")
    nodes = [n for n in function.body if isinstance(n, ast.Assign)
             and any(isinstance(t, ast.Name) and t.id in {"sealed", "committed"}
                     for t in n.targets)]
    assert len(nodes) == 2
    commits = []
    role = CollaborationRole(role="FullModel", service="/Inference", artifact="/ARTIFACT/model")
    value = SimpleNamespace(digest=lambda: DIGEST)
    namespace = dict(
        SealedCollaborationPlan=SealedCollaborationPlan, DATA_DRIVEN_V2=DATA_DRIVEN_V2,
        closed=SimpleNamespace(digest=DIGEST), placement_input=value, proposal=value,
        strategy_identity_digest=DIGEST, roles=[role], dependencies=[],
        key_scopes={}, role_scopes={"FullModel": ()},
        providers_by_role={"FullModel": "/provider"},
        artifact_names={"FullModel": "/ARTIFACT/model"}, artifact_fetch_names=fetches,
        scope_key_data_names={}, assignment_payloads={"FullModel": b"assignment"},
        collaboration=SimpleNamespace(commit_plan=lambda **kwargs: commits.append(kwargs) or True))
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), "exec"), namespace)
    return namespace["sealed"], commits[0]


@pytest.mark.parametrize("fetch", ["/APP/encrypted-root", ""])
def test_v3_sealing_preserves_canonical_identity_and_commits_transport(fetch):
    supplied = {"FullModel": fetch}
    sealed, committed = seal_and_commit(supplied)
    assert dict(sealed.artifact_data_names) == {"FullModel": "/ARTIFACT/model"}
    assert committed["artifact_data_names"] == {"FullModel": fetch}
    original_digest = sealed.plan_digest
    supplied["FullModel"] = "/changed"
    assert sealed.plan_digest == original_digest
    with pytest.raises(TypeError):
        sealed.artifact_fetch_data_names["FullModel"] = "/changed"
    assert replace(sealed, artifact_fetch_data_names={"FullModel": "/changed"}).plan_digest != original_digest


def test_omitted_fetch_map_keeps_legacy_reference():
    sealed, committed = seal_and_commit({})
    assert dict(sealed.artifact_fetch_data_names) == dict(sealed.artifact_data_names)
    assert committed["artifact_data_names"] == dict(sealed.artifact_data_names)


@pytest.mark.parametrize("fetches", [
    {"unknown": "/APP/root"}, {"FullModel": "relative"},
    {"FullModel": None}, {"FullModel": False}, {"FullModel": 0},
    {"FullModel": "/APP/root\n"}, {"FullModel": "/APP/\x00root"},
])
def test_invalid_fetch_map_is_rejected_before_commit(fetches):
    with pytest.raises(ValueError, match="fetch references"):
        seal_and_commit(fetches)
