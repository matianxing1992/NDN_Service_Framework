"""Contract tests for the Spec175 MiniNDN role/provider layout."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"


def _runner_module():
    spec = importlib.util.spec_from_file_location("spec175_minindn_runner", RUNNER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_four_stage_layout_assigns_distinct_nodes_providers_and_repositories():
    runner = _runner_module()
    runner.configure_stage_layout(4)

    assert runner.STAGE_NODES == ["ucla", "arizona", "wustl", "neu"]
    assert runner.STAGE_PROVIDER_IDS == ["", "1", "2", "3"]
    assert runner.STAGE_IDENTITIES == [
        "/example/llm-pipeline/provider",
        "/example/llm-pipeline/provider/1",
        "/example/llm-pipeline/provider/2",
        "/example/llm-pipeline/provider/3",
    ]
    assert runner.REPO_IDENTITIES == [
        "/example/llm-pipeline/repo",
        "/example/llm-pipeline/repo/1",
        "/example/llm-pipeline/repo/2",
        "/example/llm-pipeline/repo/3",
    ]
    assert len(set(runner.STAGE_NODES)) == 4
    assert len(set(runner.STAGE_IDENTITIES)) == 4


def test_spec175_host_layout_separates_controller_repo_user_router_and_providers():
    runner = _runner_module()
    runner.configure_spec175_host_layout()

    assert runner.CONTROLLER_NODE == "c"
    assert runner.REPOSITORY_NODE == "repo"
    assert runner.USER_NODE == "u"
    assert runner.ROUTER_NODE == "a"
    assert runner.STAGE_NODES == ["p0", "p1", "p2", "p3"]
    assert runner.REPO_IDENTITIES == []
    assert len(set([
        runner.CONTROLLER_NODE,
        runner.REPOSITORY_NODE,
        runner.USER_NODE,
        runner.ROUTER_NODE,
        *runner.STAGE_NODES,
    ])) == 8


def test_tiny_dataflow_derives_contiguous_repository_stage_indices(
        tmp_path: Path):
    runner = _runner_module()
    runner.configure_spec175_host_layout()
    runner.OUT = tmp_path
    fixture = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"
    args = runner.build_parser().parse_args([
        "--runtime", "tiny-onnx",
        "--stages", "4",
        "--tiny-onnx-fixture-root", str(fixture),
        "--output-dir", str(tmp_path),
    ])

    prepared = runner.prepare_tiny_selection_dataflow(args)
    manifest = json.loads(
        Path(str(prepared["repoStageManifest"])).read_text(encoding="utf-8"))

    assert [stage["stageIndex"] for stage in manifest["stages"]] == [0, 1, 2, 3]
    assert [stage["role"] for stage in manifest["stages"]] == [
        f"/LLM/Pipeline/Stage/{index}" for index in range(4)
    ]


@pytest.mark.parametrize("stage_count", [0, 1, 5, 8])
def test_stage_layout_rejects_unsupported_topology_size(stage_count: int):
    runner = _runner_module()
    with pytest.raises(ValueError, match="stage count"):
        runner.configure_stage_layout(stage_count)
