from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/spec180_inventory.py"


def load_inventory():
    spec = importlib.util.spec_from_file_location("spec180_inventory", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _executable(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(0o755)


def _fixture_root(tmp_path: Path) -> Path:
    _executable(tmp_path / "build/unit-tests")
    _executable(tmp_path / "build/integration-tests")
    for case, relative, _args in load_inventory().DEFAULT_CASES:
        del case
        target = tmp_path / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("# fixture\n", encoding="utf-8")
    for name in ("test_spec180_alpha.py", "test_spec180_beta.py"):
        target = tmp_path / "tests/python" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("def test_fixture():\n    pass\n", encoding="utf-8")
    return tmp_path


def test_boost_listing_returns_nested_and_top_level_leaves():
    module = load_inventory()
    listing = """
SuiteA*
    TestOne*
    TestTwo*
TopLevel*
SuiteB*
    Nested*
"""
    assert module.parse_boost_list(listing) == (
        "SuiteA/TestOne", "SuiteA/TestTwo", "SuiteB/Nested", "TopLevel",
    )


def test_pytest_collection_is_bound_to_repository_paths(tmp_path: Path):
    module = load_inventory()
    target = tmp_path / "tests/python/test_spec180_alpha.py"
    target.parent.mkdir(parents=True)
    target.write_text("def test_x():\n    pass\n", encoding="utf-8")
    output = "tests/python/test_spec180_alpha.py::test_x\n1 test collected\n"
    assert module.parse_pytest_collect_output(output, tmp_path) == (
        "tests/python/test_spec180_alpha.py::test_x",
    )


def test_build_inventory_is_candidate_bound_and_complete(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    inventory = module.build_inventory(
        root,
        candidate_id="candidate-test",
        candidate_digest="sha256:" + "a" * 64,
        source_revision="b" * 40,
        effective_config_digest="sha256:" + "c" * 64,
        integration_listing=(
            "NdnSvsSmoke*\n"
            "    DummyFacesDeliverV2RequestPublication*\n"
            "NdnSvsStandalone*\n"
        ),
        python_selectors=(
            "tests/python/test_spec180_beta.py::test_fixture",
            "tests/python/test_spec180_alpha.py::test_fixture",
        ),
    )
    assert inventory["schema"] == module.SCHEMA
    assert inventory["inventoryDigest"].startswith("sha256:")
    assert {entry["case"] for entry in inventory["entries"]
            if entry["kind"] == "minindn-case"} == {
                "Y-A", "Y-B", "Y-N",
            }
    assert any(entry["kind"] == "cpp-suite" for entry in inventory["entries"])
    assert any(entry["kind"] == "cpp-selector" for entry in inventory["entries"])
    assert all(entry["command"][2:] == ["--case", entry["case"]]
               for entry in inventory["entries"] if entry["kind"] == "minindn-case")
    assert module.validate_inventory(inventory)["inventoryDigest"] == inventory[
        "inventoryDigest"]


def test_missing_formal_case_fails_before_inventory_creation(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    (root / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py").unlink()
    with pytest.raises(module.InventoryError, match="FILE_MISSING:case-Y-A"):
        module.build_inventory(
            root,
            candidate_id="candidate-test",
            candidate_digest="sha256:" + "a" * 64,
            source_revision="b" * 40,
            effective_config_digest="sha256:" + "c" * 64,
            integration_listing="Suite*\n    Test*\n",
            python_selectors=(
                "tests/python/test_spec180_alpha.py::test_fixture",
            ),
        )


def test_inventory_digest_tamper_is_rejected(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    inventory = module.build_inventory(
        root,
        candidate_id="candidate-test",
        candidate_digest="sha256:" + "a" * 64,
        source_revision="b" * 40,
        effective_config_digest="sha256:" + "c" * 64,
        integration_listing="Suite*\n    Test*\n",
        python_selectors=(
            "tests/python/test_spec180_alpha.py::test_fixture",
        ),
    )
    inventory["entries"][0]["command"].append("--tampered")
    with pytest.raises(module.InventoryError, match="ENTRY_COMMAND_DIGEST_MISMATCH"):
        module.validate_inventory(inventory)
