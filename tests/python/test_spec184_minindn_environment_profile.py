from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"


def load_pipeline():
    spec = importlib.util.spec_from_file_location("spec184_environment_runner", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_profile_is_strict_and_resolves_repository_relative_paths(tmp_path: Path):
    module = load_pipeline()
    profile_path = tmp_path / "environment.json"
    profile_path.write_text(json.dumps({
        "schema": module.ENVIRONMENT_PROFILE_SCHEMA,
        "profileId": "test-profile",
        "topologyFile": "Experiments/Topology/AI_Lab.conf",
        "stageNodes": ["ucla", "arizona", "wustl", "neu"],
        "minindnRoot": str(tmp_path / "minindn"),
    }), encoding="utf-8")
    profile = module.load_environment_profile(profile_path)
    assert profile is not None
    assert profile["profileId"] == "test-profile"
    settings = profile["settings"]
    assert settings["topologyFile"] == str(
        (ROOT / "Experiments/Topology/AI_Lab.conf").resolve())
    assert settings["stageNodes"] == ["ucla", "arizona", "wustl", "neu"]
    assert len(profile["sha256"]) == 64

    bad = dict(json.loads(profile_path.read_text(encoding="utf-8")))
    bad["unexpected"] = True
    profile_path.write_text(json.dumps(bad), encoding="utf-8")
    with pytest.raises(ValueError, match="unknown fields"):
        module.load_environment_profile(profile_path)


def test_profile_defaults_and_cli_overrides_preserve_one_runner_path(tmp_path: Path):
    module = load_pipeline()
    original_nodes = list(module.STAGE_NODE_CANDIDATES)
    profile_path = tmp_path / "environment.json"
    profile_path.write_text(json.dumps({
        "schema": module.ENVIRONMENT_PROFILE_SCHEMA,
        "profileId": "test-profile",
        "topologyFile": "Experiments/Topology/AI_Lab.conf",
        "stageNodes": ["ucla", "arizona", "wustl", "neu"],
        "outputDir": str(tmp_path / "profile-output"),
    }), encoding="utf-8")
    try:
        args = module.build_parser().parse_args([
            "--environment-profile", str(profile_path),
            "--output-dir", str(tmp_path / "cli-output"),
            "--stage-nodes", "ucla,arizona",
            "--stages", "2",
        ])
        module.apply_environment_profile(args)
        assert args.output_dir == str(tmp_path / "cli-output")
        assert module.STAGE_NODE_CANDIDATES == ["ucla", "arizona"]
        assert args.topology_file == str(
            (ROOT / "Experiments/Topology/AI_Lab.conf").resolve())
        module.configure_stage_layout(args.stages)
        module.validate_environment_inputs(args)
        assert module.STAGE_NODES == ["ucla", "arizona"]
    finally:
        module.STAGE_NODE_CANDIDATES[:] = original_nodes


def test_stage_nodes_reject_duplicate_or_unsafe_names():
    module = load_pipeline()
    with pytest.raises(ValueError, match="unique"):
        module.parse_stage_nodes("ucla,ucla")
    with pytest.raises(ValueError, match="empty entries"):
        module.parse_stage_nodes("ucla,,arizona")
    with pytest.raises(ValueError, match="only letters"):
        module.parse_stage_nodes("ucla,stage/1")
