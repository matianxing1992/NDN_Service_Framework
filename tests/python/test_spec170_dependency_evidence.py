import json
from pathlib import Path

from Experiments.spec170_dependency_evidence import (
    collect_dependency_execution_evidence,
)


def _write_plan(path: Path) -> None:
    path.write_text(json.dumps({
        "services": [{
            "service": "/Inference/NativeTracer",
            "dependencies": [{
                "keyScope": "backbone-to-head0",
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/0"],
            }],
        }],
    }), encoding="utf-8")


def test_dependency_evidence_accepts_matching_nonempty_transfer(tmp_path: Path) -> None:
    plan = tmp_path / "plan.json"
    log = tmp_path / "provider.log"
    _write_plan(plan)
    log.write_text(
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=/Backbone "
        "scope=backbone-to-head0 planned_name=/x bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=/Head/Shard/0 "
        "producer=/Backbone scope=backbone-to-head0 planned_name=/x bytes=8\n",
        encoding="utf-8",
    )

    result = collect_dependency_execution_evidence([log], plan)

    assert result["status"] == "executed"
    assert result["completeEdgeCount"] == 1


def test_dependency_evidence_uses_actual_data_name_when_planned_name_is_flag(
        tmp_path: Path) -> None:
    """Python runtime traces use planned_name=true/false plus data_name=URI."""
    plan = tmp_path / "plan.json"
    log = tmp_path / "provider.log"
    _write_plan(plan)
    log.write_text(
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=/Backbone "
        "scope=backbone-to-head0 planned_name=false "
        "data_name=/activation/session/backbone-to-head0 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=/Head/Shard/0 "
        "producer=/Backbone scope=backbone-to-head0 planned_name=true "
        "data_name=/activation/session/backbone-to-head0 bytes=8\n",
        encoding="utf-8",
    )

    result = collect_dependency_execution_evidence([log], plan)

    assert result["status"] == "executed"
    assert result["nameMismatches"] == []


def test_dependency_evidence_rejects_missing_consumer_fetch(tmp_path: Path) -> None:
    plan = tmp_path / "plan.json"
    log = tmp_path / "provider.log"
    _write_plan(plan)
    log.write_text(
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=/Backbone "
        "scope=backbone-to-head0 planned_name=/x bytes=8\n",
        encoding="utf-8",
    )

    result = collect_dependency_execution_evidence([log], plan)

    assert result["status"] == "incomplete"
    assert result["missingFetches"] == [
        "/Backbone->/Head/Shard/0:backbone-to-head0"]


def test_dependency_evidence_matches_runtime_scope_for_redistribution(
        tmp_path: Path) -> None:
    plan = tmp_path / "plan.json"
    log = tmp_path / "provider.log"
    plan.write_text(json.dumps({
        "services": [{
            "service": "/Inference/NativeTracer",
            "dependencies": [{
                "keyScope": "boundary-1",
                "producers": ["S1R0", "S1R1"],
                "consumers": ["S2R0"],
                "redistributions": [{
                    "kind": "gather",
                    "producerRanks": [0, 1],
                    "consumerRanks": [0],
                }],
            }],
        }],
    }), encoding="utf-8")
    log.write_text(
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=S1R0 "
        "scope=boundary-1/from/S1R0 planned_name=/x0 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=S2R0 producer=S1R0 "
        "scope=boundary-1/from/S1R0 planned_name=/x0 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=S1R1 "
        "scope=boundary-1/from/S1R1 planned_name=/x1 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=S2R0 producer=S1R1 "
        "scope=boundary-1/from/S1R1 planned_name=/x1 bytes=8\n",
        encoding="utf-8",
    )

    result = collect_dependency_execution_evidence([log], plan)

    assert result["status"] == "executed"
    assert result["completeEdgeCount"] == 2
    assert [edge["scope"] for edge in result["edges"]] == [
        "boundary-1/from/S1R0", "boundary-1/from/S1R1"]


def test_dependency_evidence_accepts_logical_producer_scope_for_redistribution(
        tmp_path: Path) -> None:
    plan = tmp_path / "plan.json"
    log = tmp_path / "provider.log"
    plan.write_text(json.dumps({
        "services": [{
            "service": "/Inference/NativeTracer",
            "dependencies": [{
                "keyScope": "boundary-1",
                "producers": ["S1R0", "S1R1"],
                "consumers": ["S2R0"],
                "redistributions": [{"kind": "gather"}],
            }],
        }],
    }), encoding="utf-8")
    log.write_text(
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=S1R0 "
        "scope=boundary-1 planned_name=/x0 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=S2R0 producer=S1R0 "
        "scope=boundary-1/from/S1R0 planned_name=/x0 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING producer=S1R1 "
        "scope=boundary-1 planned_name=/x1 bytes=8\n"
        "NDNSF_DI_DEPENDENCY_INPUT_TIMING role=S2R0 producer=S1R1 "
        "scope=boundary-1/from/S1R1 planned_name=/x1 bytes=8\n",
        encoding="utf-8",
    )

    result = collect_dependency_execution_evidence([log], plan)

    assert result["status"] == "executed"
    assert result["missingPublications"] == []
