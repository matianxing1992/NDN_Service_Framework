"""Static checks of the portable definition; never invoke Apptainer or APT."""
import ast
import importlib.util
from pathlib import Path
import re
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[3]
TEMPLATE = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/templates/development-runtime.def.in"
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
    text = path.read_text()
    assert "/opt/ndnsf-stage/lib/libndnsf-distributed-inference.so" in text
    assert "/opt/ndnsf-app/bin" in text
    assert "test ! -e /opt/ndnsf-app" in text
    assert "test ! -L /opt/ndnsf-app" in text
    assert "install -m 0755 /opt/ndnsf-candidate/lib/libndnsf-distributed-inference.so" in text
    for name in ("libndn-service-framework.so.0.1.0", "libndn-svs.so.0.1.0",
                 "libnac-abe.so", "libndnsd.so.0.1.0", "libopenabe.so",
                 "librelic.so", "librelic_ec.so"):
        assert f"/opt/ndnsf-candidate/lib/{name}" in text
    assert "install -m 0755 /opt/ndnsf-candidate/bin/di-native-provider /opt/ndnsf-app/bin/di-native-provider" in text
    assert "install -m 0755 /opt/ndnsf-candidate/bin/di-native-fault-provider /opt/ndnsf-app/bin/di-native-fault-provider" in text
    assert "install -m 0755 /opt/ndnsf-candidate/bin/App_ServiceController /opt/ndnsf-app/bin/App_ServiceController" in text
    assert "cp -aL /opt/ndnsf-candidate/python/. /opt/ndnsf-app/python/" in text
    assert "cp -aL /opt/ndnsf-candidate/replay/. /opt/ndnsf-app/replay/" in text
    assert "cp -aL /opt/ndnsf-candidate/lib/. /opt/ndnsf-app/lib/" not in text
    assert "APP_NATIVE_LIBRARY_CLOSURE_MISMATCH" in text
    assert "APP_LAYOUT_VERIFY=ndnsf-app-v2" in text
    assert "NDNSF_RUNTIME_RPATH='$ORIGIN/../../lib:/opt/ndn-base/lib'" in text
    assert "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib:/opt/ndn-base/lib" in text
    assert "export NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-stage" in text


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


def test_builder_uses_isolated_sdk_as_app_input(tmp_path):
    text = render(tmp_path).read_text()
    builder = {s.name: s for s in boundary._parse_stages(text)}['builder'].sections['post']
    stage = builder.index('"/opt/ndn-base/sdk/lib/$library" "/opt/ndnsf-stage/lib/$library"')
    assert stage < builder.index('/usr/bin/cmake -S /src/nac-abe')
    assert '-DCMAKE_LIBRARY_PATH=/opt/ndnsf-stage/lib' in builder
    assert 'export LIBRARY_PATH=/opt/ndnsf-stage/lib:/opt/ndn-base/lib' in builder
    for line in builder.splitlines():
        if 'export LD_LIBRARY_PATH=' in line:
            assert '/opt/ndn-base/sdk' not in line
    assert '/usr/local/lib/libopenabe.so /opt/ndnsf-stage' not in builder
    assert 'python3.10-dev' not in builder
    assert "Path(sysconfig.get_paths()['include'], 'Python.h').is_file()" in builder
    assert '-Wl,-rpath,/opt/ndnsf-di/current/lib' not in builder
