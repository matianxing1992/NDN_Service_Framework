"""Exercise the inherited inventory against Spec181's actual acceptance scope."""

from pathlib import Path

import pytest

from test_spec180_inventory import _fixture_root, load_inventory


def test_spec181_generated_inventory_requires_only_yolo_network_cases(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    inventory = module.build_inventory(
        root,
        candidate_id="spec181-candidate",
        candidate_digest="sha256:" + "a" * 64,
        source_revision="b" * 40,
        environment={},
        integration_listing="Suite*\n    Test*\n",
        python_selectors=("tests/python/test_spec180_alpha.py::test_fixture",),
    )
    cases = {entry["case"] for entry in inventory["entries"]
             if entry["kind"] == "minindn-case"}
    assert cases == {"Y-A", "Y-B", "Y-N"}


def test_spec181_discovery_includes_protected_grant_regressions(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    target = root / "tests/python/test_spec181_protected_fixture.py"
    target.write_text("def test_protected_grant():\n    pass\n", encoding="utf-8")
    selectors = module.discover_python_selectors(root)
    assert "tests/python/test_spec181_protected_fixture.py::test_protected_grant" in selectors
    assert "tests/python/test_spec180_alpha.py::test_fixture" in selectors


def test_inventory_cannot_rebind_a_registered_case_to_another_source(tmp_path: Path):
    module = load_inventory()
    root = _fixture_root(tmp_path)
    alternate = root / "Experiments/alternate.py"
    alternate.write_text("# alternate source\n", encoding="utf-8")
    cases = tuple((case, "Experiments/alternate.py" if case == "Y-B" else path, args)
                  for case, path, args in module.DEFAULT_CASES)
    with pytest.raises(module.InventoryError, match="CASE_CONTRACT_MISMATCH:Y-B"):
        module.build_inventory(
            root, candidate_id="spec181-candidate",
            candidate_digest="sha256:" + "a" * 64, source_revision="b" * 40,
            environment={},
            integration_listing="Suite*\n    Test*\n",
            python_selectors=("tests/python/test_spec180_alpha.py::test_fixture",),
            cases=cases,
        )


def test_gate_rejects_rehashed_case_source_substitution_before_children(tmp_path: Path):
    from test_spec180_local_gate import _inventory, _refresh_inventory_digest

    module, gate, root, inventory = _inventory(tmp_path)
    entry = next(item for item in inventory["entries"] if item["case"] == "Y-A")
    original = root / entry["path"]
    alternate = root / "Experiments/alternate.py"
    alternate.write_bytes(original.read_bytes())
    entry["path"] = "Experiments/alternate.py"
    entry["command"][1] = entry["path"]
    entry["commandDigest"] = module.canonical_digest(entry["command"])
    entry["artifactSha256"] = module.digest_bytes(alternate.read_bytes())
    _refresh_inventory_digest(module, inventory)
    output = tmp_path / "evidence"
    with pytest.raises(gate.LocalGateError, match="CASE_CONTRACT_MISMATCH:Y-A"):
        gate.run_local_gate(inventory, root=root, output_root=output, environment={})
    assert not output.exists()


@pytest.mark.parametrize("entry_id", ["../outside", "/absolute", ".."])
def test_inventory_rejects_entry_ids_that_escape_evidence_root(tmp_path: Path, entry_id: str):
    from test_spec180_local_gate import _inventory, _refresh_inventory_digest

    module, _gate, _root, inventory = _inventory(tmp_path)
    inventory["entries"][0]["id"] = entry_id
    _refresh_inventory_digest(module, inventory)
    with pytest.raises(module.InventoryError, match="DUPLICATE_OR_INVALID_ENTRY_ID"):
        module.validate_inventory(inventory)
