"""Static checks of the portable definition; never invoke Apptainer or APT."""
import ast
import importlib.util
from pathlib import Path
import re
import subprocess
import tarfile

import pytest


ROOT = Path(__file__).resolve().parents[3]
TEMPLATE = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/templates/development-runtime.def.in"
PREFLIGHT = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-development-sif.py"
spec = importlib.util.spec_from_file_location(
    "portable_template_boundary", ROOT / "packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py")
boundary = importlib.util.module_from_spec(spec)
# dataclasses resolves annotation context through sys.modules on Python 3.8.
import sys
sys.modules[spec.name] = boundary
try:
    spec.loader.exec_module(boundary)
finally:
    del sys.modules[spec.name]

preflight_spec = importlib.util.spec_from_file_location(
    "spec186_development_preflight", PREFLIGHT)
preflight = importlib.util.module_from_spec(preflight_spec)
sys.modules[preflight_spec.name] = preflight
try:
    preflight_spec.loader.exec_module(preflight)
finally:
    del sys.modules[preflight_spec.name]


def render(tmp_path):
    text = TEMPLATE.read_text()
    values = {"@BUNDLE@": str(tmp_path / "bundle"),
              "@BASE_SIF@": str(tmp_path / "base.sif"),
              "@SEAL_DIGEST@": "sha256:" + "a" * 64,
              "@RELEASE@": "unit-static-only"}
    assert set(re.findall(r"@[A-Z_]+@", text)) == set(values)
    for key, value in values.items():
        text = text.replace(key, value)
    path = tmp_path / "runtime.def"
    path.write_text(text)
    return path


def test_rendered_template_preserves_container_build_boundary(tmp_path):
    path = render(tmp_path)
    result = boundary.validate_definition(path)
    assert result["status"] == "PASS" and result["hostBinaryInputs"] == []
    assert result["baseImage"] == str(tmp_path / "base.sif")


@pytest.mark.parametrize("stage", ["builder", "final"])
def test_stage_shell_and_embedded_python_parse_without_execution(tmp_path, stage):
    path = render(tmp_path)
    parsed = {item.name: item for item in boundary._parse_stages(path.read_text())}
    post = parsed[stage].sections["post"]
    subprocess.run(["/bin/sh", "-n"], input=post, text=True, capture_output=True, check=True)
    snippets = re.findall(r"<<'PY'\n(.*?)\nPY(?:\n|$)", post, flags=re.S)
    assert snippets
    for index, snippet in enumerate(snippets):
        ast.parse(snippet, filename=stage + "-python-" + str(index))


def test_boundary_rejects_a_host_native_payload_counterfactual(tmp_path):
    path = render(tmp_path)
    path.write_text(path.read_text().replace("%files\n", "%files\n    /unavailable/host.so /build-input/host.so\n", 1))
    with pytest.raises(boundary.Spec170BuildBoundaryError, match="WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT"):
        boundary.validate_definition(path)


def test_spec186_preflight_is_wired_before_expensive_build():
    source = PREFLIGHT.read_text(encoding="utf-8")
    ast.parse(source, filename=str(PREFLIGHT))
    assert "WORKSPACE_TARGET_INPUT_MISSING" in source
    assert "NUMPY_PRIVATE_LIB_SET" in source
    assert "NUMPY_FINAL_RPATH_RESTORE_MISSING" in source
    assert "NUMPY_BASE_IMPORT_PASS" in source
    assert "clang-10" in TEMPLATE.read_text()
    assert "/usr/bin/clang++-10" in TEMPLATE.read_text()
    assert "SPEC186_BASE_CAPABILITY_BEGIN" in TEMPLATE.read_text()
    assert "SPEC186_BASE_CAPABILITY_END" in TEMPLATE.read_text()
    assert "--toolchain-root=/usr" in TEMPLATE.read_text()
    assert TEMPLATE.read_text().index("export CXX=/usr/bin/clang++-10") < TEMPLATE.read_text().index("./waf -j1 -v --targets=")
    assert "cp -a /src/ndnsf/Experiments/TigerCluster/jobs/spec180" in TEMPLATE.read_text()
    assert "cp -a /src/ndnsf/specs/162-itiger-qwen36-generation" in TEMPLATE.read_text()
    assert "cp -a /src/ndnsf/packaging/ndnsf-di-container/jobs/spec180" not in TEMPLATE.read_text()
    assert "/var/lib/dpkg/updates" in TEMPLATE.read_text()
    build_script = (ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh").read_text()
    assert "preflight-development-sif.py" in build_script
    assert build_script.index("preflight_json") < build_script.index('echo "LOCAL_SIF_BUILD_START')
    assert build_script.index("base_sif=''") < build_script.index('preflight_args=')
    assert build_script.index('base_sif=$(awk') < build_script.index('preflight_args=')
    assert "--verify-existing" in build_script
    assert "local-apptainer-existing-sif-verify" in build_script
    subprocess.run(["/bin/bash", "-n"], input=build_script, text=True, check=True)


def test_waf_does_not_default_to_a_developer_temp_toolchain():
    source = (ROOT / "wscript").read_text(encoding="utf-8")
    assert ".codex-tmp/spec182-t001-dependencies" not in source
    assert "NDNSF_RUST_PREFIX is required" in source
    assert "NDNSF_CARGO_HOME is required" in source
    assert "or os.path.join(top, 'build', 'tokenizer-bridge-target')" in source


def test_preflight_rejects_workspace_archive_omitting_consumed_source(tmp_path):
    definition = render(tmp_path)
    workspace = tmp_path / "workspace.tar"
    with tarfile.open(workspace, "w") as archive:
        info = tarfile.TarInfo("Experiments/NDNSF_DI_YoloAckDriven_Minindn.py")
        info.size = 0
        archive.addfile(info)
    with pytest.raises(SystemExit, match="SPEC186_PREFLIGHT_WORKSPACE_CONSUMER_PATH_MISSING"):
        preflight.check_workspace_consumers(definition, workspace)


def test_preflight_reports_all_consumed_workspace_paths(tmp_path):
    definition = render(tmp_path)
    workspace = tmp_path / "workspace.tar"
    paths = {
        "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py",
        "Experiments/TigerCluster/jobs/spec180",
        "NDNSF-DistributedInference/ndnsf_distributed_inference",
        "NDNSF-DistributedRepo/pythonWrapper",
        "pythonWrapper",
        "tools/ndnsf-di",
        "tests/fixtures",
        "specs/162-itiger-qwen36-generation",
        "examples/trust-schema.conf",
        "examples/python",
    }
    with tarfile.open(workspace, "w") as archive:
        for path in paths:
            info = tarfile.TarInfo(path)
            info.size = 0
            archive.addfile(info)
    consumed = preflight.check_workspace_consumers(definition, workspace)
    assert "Experiments/TigerCluster/jobs/spec180" in consumed
    assert "NDNSF-DistributedInference/ndnsf_distributed_inference" in consumed


def test_preflight_extracts_base_capability_predicates(tmp_path):
    definition = render(tmp_path)
    checks = set(preflight._base_capability_tests(definition))
    assert ("-x", "/usr/bin/clang-10") not in checks
    assert ("-f", "/opt/onnx/lib/libonnx.a") in checks
    assert ("-x", "/opt/rust-prefix/bin/cargo") in checks
    assert ("-x", "/opt/rust-prefix/bin/rustc") in checks
    assert ("-d", "/opt/cargo-home/registry/src") in checks
    assert all(not path.startswith("/opt/ndnsf-stage") for _, path in checks)


def test_successful_gpu_template_is_a_required_comparison_reference():
    reference = ROOT / "Experiments/TigerCluster/docs/successful-tiger-gpu-template.md"
    text = reference.read_text(encoding="utf-8")
    assert "44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0" in text
    assert "Clang 10" in text and "Apptainer 1.5.3" in text
    assert "210365" in text and "210366" in text
    assert "not current Spec186 evidence" in text


def test_sif_build_docs_require_template_before_render():
    docs = (ROOT / "Experiments/TigerCluster/docs/sif-build.md").read_text(encoding="utf-8")
    skill = (ROOT / "skills/itiger-ndnsf-ops/SKILL.md").read_text(encoding="utf-8")
    for text in (docs, skill):
        assert "successful-tiger-gpu-template.md" in text
        assert "prepare-development-handoff.py render" in text or "从 `render`" in text
        assert "failure-log.md" in text
