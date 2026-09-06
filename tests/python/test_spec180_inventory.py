from __future__ import annotations

import importlib.util
import json
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
        environment={},
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
            environment={},
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
        environment={},
        integration_listing="Suite*\n    Test*\n",
        python_selectors=(
            "tests/python/test_spec180_alpha.py::test_fixture",
        ),
    )
    inventory["entries"][0]["command"].append("--tampered")
    with pytest.raises(module.InventoryError, match="ENTRY_COMMAND_DIGEST_MISMATCH"):
        module.validate_inventory(inventory)


def test_launch_configuration_binds_actual_inputs_without_exposing_env_values(tmp_path: Path):
    module = load_inventory()
    environment = {"PRIVATE_VALUE": "fixture-sensitive-value", "PATH": "/usr/bin"}
    record = module.local_launch_configuration(tmp_path, environment, 120)
    assert record == module.local_launch_configuration(tmp_path, dict(reversed(list(environment.items()))), 120)
    assert "fixture-sensitive-value" not in str(record)
    assert record["environmentDigest"] == module.canonical_digest(environment)
    for root, env, timeout in [(tmp_path / "other", environment, 120),
                               (tmp_path, {}, 120), (tmp_path, environment, 121)]:
        assert module.canonical_digest(module.local_launch_configuration(root, env, timeout)) != module.canonical_digest(record)


@pytest.mark.parametrize("environment,reason", [
    ({"SPEC180_CASE_OUTPUT_DIR": ""}, "CONFIG_RESERVED_ENVIRONMENT"),
    ({"SPEC180_RUNTIME_SIF": "/external/runtime.sif"}, "LOCAL_GATE_SIF_RUNTIME_UNSUPPORTED"),
    ({"BAD=KEY": "value"}, "CONFIG_INVALID_ENVIRONMENT"),
    ({"KEY": "bad\0value"}, "CONFIG_INVALID_ENVIRONMENT"),
    ({"KEY": 1}, "ENVIRONMENT_MUST_BE_STRING_MAP"),
])
def test_launch_configuration_rejects_unusable_or_wrong_scope_environment(tmp_path: Path, environment, reason):
    module = load_inventory()
    with pytest.raises(module.InventoryError, match=reason):
        module.local_launch_configuration(tmp_path, environment, 120)


def test_inventory_rejects_caller_config_digest_before_discovery(tmp_path: Path):
    module = load_inventory()
    with pytest.raises(module.InventoryError, match="EFFECTIVE_CONFIG_DIGEST_MISMATCH"):
        module.build_inventory(tmp_path, candidate_id="candidate-test",
                               candidate_digest="sha256:" + "a" * 64,
                               source_revision="b" * 40, environment={},
                               effective_config_digest="sha256:" + "c" * 64)


@pytest.mark.parametrize("mutation,reason", [
    ("missing", "LOCAL_CASE_CONFIG_INCOMPLETE"),
    ("empty", "LOCAL_CASE_CONFIG_INCOMPLETE"),
    ("shared", "LOCAL_CASE_CONFIG_AMBIGUOUS"),
    ("relative", "LOCAL_CASE_CONFIG_NOT_ABSOLUTE"),
])
def test_case_config_requires_one_complete_unambiguous_source(tmp_path, mutation, reason):
    module = load_inventory()
    environment = {"SPEC181_LOCAL_CONFIG_Y_" + case: str(tmp_path / (case + ".json"))
                   for case in ("A", "B", "N")}
    if mutation == "missing":
        del environment["SPEC181_LOCAL_CONFIG_Y_B"]
    elif mutation == "empty":
        environment["SPEC181_LOCAL_CONFIG_Y_B"] = ""
    elif mutation == "shared":
        environment["SPEC180_YOLO_CONFIG"] = str(tmp_path / "shared.json")
    else:
        environment["SPEC181_LOCAL_CONFIG_Y_B"] = "relative.json"
    with pytest.raises(module.InventoryError, match=reason):
        module.local_launch_configuration(tmp_path, environment, 120)


@pytest.mark.parametrize("case", ["A", "B", "N"])
def test_case_config_bytes_are_bound_even_before_that_case_runs(tmp_path, case):
    module = load_inventory()
    environment = {}
    for name in ("A", "B", "N"):
        path = tmp_path / (name + ".json")
        path.write_text(json.dumps({"case": name, "roles": [name]}))
        environment["SPEC181_LOCAL_CONFIG_Y_" + name] = str(path)
    module.local_launch_configuration(tmp_path, environment, 120)
    before = module.local_input_identity(tmp_path, environment)
    Path(environment["SPEC181_LOCAL_CONFIG_Y_" + case]).write_text('{"roles": []}')
    after = module.local_input_identity(tmp_path, environment)
    assert before != after
    for name in ("A", "B", "N"):
        key = "SPEC181_LOCAL_CONFIG_Y_" + name
        assert (before["inputs"][key] == after["inputs"][key]) == (name != case)


@pytest.mark.parametrize("mutation", ["bytes", "mode", "target"])
def test_input_identity_binds_declared_checkpoint(tmp_path, mutation):
    module = load_inventory()
    checkpoint = tmp_path / "checkpoint.pt"
    checkpoint.write_bytes(b"fixed checkpoint fixture")
    checkpoint.chmod(0o600)
    alias = tmp_path / "selected.pt"
    alias.symlink_to(checkpoint)
    environment = {"SPEC180_YOLO_CHECKPOINT": str(alias)}
    before = module.local_input_identity(tmp_path, environment)
    if mutation == "bytes":
        checkpoint.write_bytes(b"changed checkpoint fixture")
    elif mutation == "mode":
        checkpoint.chmod(0o644)
    else:
        other = tmp_path / "other.pt"
        other.write_bytes(checkpoint.read_bytes())
        other.chmod(0o600)
        alias.unlink()
        alias.symlink_to(other)
    after = module.local_input_identity(tmp_path, environment)
    assert before != after
    assert "fixed checkpoint fixture" not in json.dumps(before)


@pytest.mark.parametrize("layout", ["beside", "contracts", "spec-beside"])
def test_input_identity_binds_registry_public_files_using_consumer_paths(tmp_path, layout):
    module = load_inventory()
    feature = tmp_path / "feature"
    registry = feature / ("contracts/registry.json" if layout != "beside" else "registry.json")
    registry.parent.mkdir(parents=True)
    if layout == "spec-beside":
        (registry.parent / "spec.md").write_text("fixture feature")
    catalogue_root = registry.parent if layout != "contracts" else feature
    catalogue_key = catalogue_root / "catalogue.pub"
    catalogue_key.write_bytes(b"catalogue public fixture")
    policy_key = registry.resolve().parent.parent / "policy.pub"
    policy_key.write_bytes(b"policy public fixture")
    model_key = catalogue_root / "model.pub"
    model_key.write_bytes(b"model public fixture")
    registry.write_text(json.dumps({
        "catalogue": {"publicKeyPath": "catalogue.pub"},
        "artifactPolicyAuthority": {"publicKeyPath": "policy.pub"},
        "modelManifest": {"publicKeyPath": "model.pub"},
    }))
    environment = {"SPEC180_YOLO_CATALOGUE_REGISTRY": str(registry)}
    before = module.local_input_identity(tmp_path, environment)
    references = before["inputs"]["SPEC180_YOLO_CATALOGUE_REGISTRY"]["referencedFiles"]
    assert set(references) == {"catalogue", "artifactPolicyAuthority", "modelManifest"}
    for name, key in (("catalogue", catalogue_key), ("artifactPolicyAuthority", policy_key),
                      ("modelManifest", model_key)):
        assert references[name]["resolvedPath"] == str(key.resolve())
        assert references[name]["sha256"] == module.digest_bytes(key.read_bytes())
        key.write_bytes(key.read_bytes() + b" changed")
        after = module.local_input_identity(tmp_path, environment)
        assert after != before
        before = after
    assert "public fixture" not in json.dumps(before)


@pytest.mark.parametrize("entry", [None, {}, {"publicKeyPath": ""}, {"publicKeyPath": []}])
def test_input_identity_rejects_invalid_registry_reference(tmp_path, entry):
    module = load_inventory()
    registry = tmp_path / "registry.json"
    registry.write_text(json.dumps({"catalogue": entry}))
    with pytest.raises(module.InventoryError, match="INPUT_REGISTRY_INVALID"):
        module.local_input_identity(tmp_path, {"SPEC180_YOLO_CATALOGUE_REGISTRY": str(registry)})


@pytest.mark.parametrize("kind", ["checkpoint", "registry-key"])
def test_input_identity_rejects_missing_checkpoint_or_registry_key(tmp_path, kind):
    module = load_inventory()
    environment = {"SPEC180_YOLO_CHECKPOINT": str(tmp_path / "missing.pt")}
    if kind == "registry-key":
        registry = tmp_path / "registry.json"
        registry.write_text(json.dumps({"catalogue": {"publicKeyPath": "missing.pub"}}))
        environment = {"SPEC180_YOLO_CATALOGUE_REGISTRY": str(registry)}
    with pytest.raises(module.InventoryError, match="INPUT_IDENTITY_UNREADABLE"):
        module.local_input_identity(tmp_path, environment)


def test_input_identity_binds_protected_default_key_and_references_without_values(tmp_path):
    module = load_inventory()
    key = tmp_path / ".config/ndnsf/spec180/artifact-policy-authority.key"
    key.parent.mkdir(parents=True)
    key.write_text("private fixture value must not be in evidence")
    mapping = tmp_path / "keys.json"
    mapping.write_text(json.dumps({"provider": str(key)}))
    environment = {"HOME": str(tmp_path), "SPEC181_PROTECTION_EPOCH": "protected-v1",
                   "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(mapping)}
    record = module.local_input_identity(tmp_path / "source", environment)
    assert "private fixture value" not in json.dumps(record)
    inputs = record["inputs"]
    assert inputs["protectedAuthorityPrivateKey"]["sha256"] == inputs[
        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP"]["referencedFiles"]["provider"]["sha256"]
    key.write_text("changed")
    assert module.local_input_identity(tmp_path / "source", environment) != record


@pytest.mark.parametrize("kind", ["bad-map", "missing-reference", "directory-link"])
def test_input_snapshot_rejects_unreadable_or_omitted_inputs(tmp_path, kind):
    module = load_inventory()
    if kind == "directory-link":
        package = tmp_path / "package"
        package.mkdir()
        (package / "recursive").symlink_to(package, target_is_directory=True)
        environment = {"SPEC180_YOLO_CANONICAL_PACKAGE": str(package)}
        reason = "INPUT_DIRECTORY_SYMLINK"
    else:
        mapping = tmp_path / "keys.json"
        mapping.write_text(json.dumps([] if kind == "bad-map" else {"provider": str(tmp_path / "absent.key")}))
        environment = {"SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(mapping)}
        reason = "INPUT_KEY_MAP_INVALID" if kind == "bad-map" else "INPUT_IDENTITY_UNREADABLE"
    with pytest.raises(module.InventoryError, match=reason):
        module.local_input_identity(tmp_path, environment)
