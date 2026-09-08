"""Worker-to-collector handoff tests; fixtures are not runtime qualification."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from runtime.yolo_collection import CollectionHandoffError, publish_normal_handoff
from runtime.yolo_operator import OperatorError, finalize_normal_collection


def digest(value):
    return "sha256:" + hashlib.sha256(value).hexdigest()


def plan_fixture(tmp_path, *, case="local-cpu"):
    run_root = tmp_path / "results" / "run-01"
    run_root.mkdir(parents=True)
    nodes = {0, 1} if case in ('two-node-gpu', 'negative-dependency') else {0}
    for rank in nodes:
        root = run_root / ("node" + str(rank))
        root.mkdir()
        receipt = {
            "schema": "tiger-yolo-node-receipt-v3", "runId": "run-01",
            "case": case, "rank": rank,
            "planDigest": "sha256:" + "1" * 64,
            "preparationDigest": "sha256:" + str(rank + 2) * 64,
            "candidateDigest": "sha256:" + "d" * 64,
            "launches": [{"role": "user", "invocation": "0", "pid": 100 + rank,
                          "argvDigest": "sha256:" + "b" * 64,
                          "logPath": "logs/user-0.log", "logDigest": "sha256:" + "c" * 64,
                          "logBytes": 0, "launchNonce": None}],
            "cleanup": [], "cleanupSummary": {},
            "qualification": "NODE_CLEANUP_COMPONENT_ONLY",
        }
        (root / "node-receipt.json").write_text(json.dumps(receipt))
        if case != "local-cpu":
            (root / "slurm-allocation.json").write_text("{}")
            (root / "gpu-probe.json").write_text("{}")
    plan = {"schema": "tiger-yolo-run-plan-v1", "runId": "run-01", "case": case,
            "output": str(run_root)}
    return plan, run_root, nodes


def references(tmp_path, count):
    output = []
    for index in range(count):
        package = tmp_path / ("package-" + str(index))
        repository = tmp_path / ("repository-" + str(index))
        package.mkdir()
        repository.mkdir()
        output.append({"package": str(package), "repository": str(repository),
                       "inputSize": 640})
    return output


def kwargs(tmp_path, plan, run_root, nodes):
    return dict(
        path=run_root / "collection-input.json", plan=plan,
        node_roots={rank: run_root / ("node" + str(rank)) for rank in nodes},
        references=references(tmp_path, 0 if plan['case'] == 'negative-dependency' else
                              4 if plan["case"] == "two-node-gpu" else 2),
        runtime_candidate_digest="sha256:" + "d" * 64,
        placement_candidate_id="placement-v1",
        placement_candidate_digest="sha256:" + "e" * 64,
        graph_digest="sha256:" + "f" * 64,
        catalogue_digest="sha256:" + "0" * 64,
        providers_by_role={role: "/run/" + role for role in
                           ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")},
        certified_graph={"graphDigest": "sha256:" + "f" * 64,
                         "roles": {role: {} for role in
                                   ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")}},
    )


def test_publish_normal_handoff_requires_all_rank_receipts(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path, case="two-node-gpu")
    arguments = kwargs(tmp_path, plan, root, nodes)
    arguments["allocation_expected"] = {"job_id": "1", "submission_key": "key",
                                         "partition": "gpu", "gpu_type": "rtx_6000"}
    (root / "node1" / "node-receipt.json").unlink()
    with pytest.raises(CollectionHandoffError, match="HANDOFF_FILE"):
        publish_normal_handoff(**arguments)
    assert not (root / "collection-input.json").exists()


def test_publish_normal_handoff_binds_real_receipts_and_is_immutable(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path)
    arguments = kwargs(tmp_path, plan, root, nodes)
    result = publish_normal_handoff(**arguments)
    handoff = json.loads((root / "collection-input.json").read_text())
    assert result["collectionInputDigest"] == digest((root / "collection-input.json").read_bytes())
    assert handoff["schema"] == "tiger-yolo-collection-input-v1"
    assert handoff["kind"] == "normal"
    assert handoff["nodes"]["0"]["preparationDigest"] == "sha256:" + "2" * 64
    assert (root / "collection-input.json").stat().st_mode & 0o777 == 0o444
    with pytest.raises(CollectionHandoffError, match="HANDOFF_OUTPUT_EXISTS"):
        publish_normal_handoff(**arguments)


def test_publish_normal_handoff_rejects_node_root_escape(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path)
    arguments = kwargs(tmp_path, plan, root, nodes)
    arguments["node_roots"] = {0: tmp_path}
    with pytest.raises(CollectionHandoffError, match="HANDOFF_NODE_ROOT_BINDING"):
        publish_normal_handoff(**arguments)


def test_publish_normal_handoff_requires_gpu_allocation_evidence(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path, case="two-node-gpu")
    arguments = kwargs(tmp_path, plan, root, nodes)
    with pytest.raises(CollectionHandoffError, match="HANDOFF_ALLOCATION_REQUIRED"):
        publish_normal_handoff(**arguments)


def test_outer_coordinator_refuses_partial_rank_return(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path)
    arguments = kwargs(tmp_path, plan, root, nodes)
    rank_results = {}
    with pytest.raises(OperatorError, match="OPERATOR_COLLECTION_RANKS"):
        finalize_normal_collection(
            plan=plan, rank_results=rank_results,
            node_roots=arguments.pop("node_roots"),
            collection_path=arguments.pop("path"), **{k: v for k, v in arguments.items()
                                                      if k != "plan"})


def test_outer_coordinator_publishes_only_after_all_rank_returns(tmp_path):
    plan, root, nodes = plan_fixture(tmp_path)
    arguments = kwargs(tmp_path, plan, root, nodes)
    rank_results = {0: {"rank": 0, "runId": "run-01", "case": "local-cpu",
                        "qualification": "NODE_CLEANUP_COMPONENT_ONLY"}}
    result = finalize_normal_collection(
        plan=plan, rank_results=rank_results,
        node_roots=arguments.pop("node_roots"),
        collection_path=arguments.pop("path"), **{k: v for k, v in arguments.items()
                                                  if k != "plan"})
    assert result["status"] == "READY"
    assert json.loads((root / "collection-input.json").read_text())["kind"] == "normal"
