from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/ndnsf-di/prepare_spec180_yolo_case.py"


def _load_tool():
    spec = importlib.util.spec_from_file_location("prepare_spec180_yolo_case", TOOL)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _manifest() -> dict:
    return {
        "catalogue": {
            "candidates": [
                {
                    "candidateId": "atomic-v1",
                    "candidateDigest": "sha256:" + "1" * 64,
                },
                {
                    "candidateId": "shared-backbone-two-shard-v1",
                    "candidateDigest": "sha256:" + "2" * 64,
                },
            ]
        }
    }


def test_prepare_y_b_writes_four_role_bundle_without_private_key_bytes(
        monkeypatch, tmp_path: Path):
    module = _load_tool()
    package = tmp_path / "package"
    package.mkdir()
    (package / "manifest.json").write_text("{}\n", encoding="utf-8")
    registry = tmp_path / "registry.json"
    registry.write_text("{}\n", encoding="utf-8")
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\nucla:\nneu:\n", encoding="utf-8")
    monkeypatch.setattr(
        module, "_load_verified_package", lambda *_: (object(), _manifest()))

    output = tmp_path / "case"
    keys = tmp_path / "private-keys"
    record = module.prepare_case(
        "Y-B", package, registry, output, keys, topology)

    assert record["candidateId"] == "shared-backbone-two-shard-v1"
    config = json.loads((output / "case-config.json").read_text())
    assert [item["roles"][0] for item in config["services"][0]["providers"]] == [
        "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"]
    repo_services = {
        item["name"]: item for item in config["services"]
        if item["name"].startswith("/NDNSF/DistributedRepo/")
    }
    assert set(repo_services) == set(module._repo_service_names())
    expected_clients = {
        module.CONTROLLER, module.USER, module.PROVIDER_PREFIX + "/Repo",
        *(item["identity"] for item in config["services"][0]["providers"]),
    }
    for service in repo_services.values():
        assert set(service["users"]) == expected_clients
        assert service["providers"] == [{
            "identity": module.PROVIDER_PREFIX + "/Repo", "roles": []}]
    private_map = json.loads((output / "offer-private-key-map.json").read_text())
    public_map = json.loads((output / "offer-public-key-map.json").read_text())
    assert len(private_map) == len(public_map) == 4
    assert all(Path(path).is_file() for path in private_map.values())
    assert all(Path(path).stat().st_mode & 0o077 == 0 for path in private_map.values())
    public_bundle = "\n".join(
        path.read_text(encoding="utf-8") for path in output.rglob("*") if path.is_file())
    assert "PRIVATE KEY" not in public_bundle


def test_prepare_rejects_private_key_root_inside_repository(
        monkeypatch, tmp_path: Path):
    module = _load_tool()
    monkeypatch.setattr(
        module, "_load_verified_package", lambda *_: (object(), _manifest()))
    topology = tmp_path / "topology.conf"
    topology.write_text("[nodes]\nmemphis:\n", encoding="utf-8")

    with pytest.raises(module.PreparationError, match="outside the repository"):
        module.prepare_case(
            "Y-A", tmp_path, tmp_path / "registry.json", tmp_path / "out",
            ROOT / ".forbidden-spec180-keys", topology)
