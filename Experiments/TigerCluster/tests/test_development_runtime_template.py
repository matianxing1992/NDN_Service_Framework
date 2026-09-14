"""Static checks of the portable definition; never invoke Apptainer or APT."""
import ast
import importlib.util
from pathlib import Path
import re
import subprocess

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
    build_script = (ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh").read_text()
    assert "preflight-development-sif.py" in build_script
    assert build_script.index("preflight_json") < build_script.index('echo "LOCAL_SIF_BUILD_START')
