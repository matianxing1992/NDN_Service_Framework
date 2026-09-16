"""Static checks of the portable definition; never invoke Apptainer or APT."""
import ast
import hashlib
import json
import os
import importlib.util
from pathlib import Path
import re
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[3]
TEMPLATE = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/templates/development-runtime.def.in"


def test_ndnsd_prefix_check_survives_system_include_filter(tmp_path):
    include = tmp_path / 'include'
    include.mkdir()
    (tmp_path / 'ndnsd.pc').write_text(
        f'includedir={include}\nName: ndnsd\nDescription: fixture\nVersion: 1\nCflags: -I${{includedir}}\n')
    env = dict(os.environ, PKG_CONFIG_LIBDIR=str(tmp_path), PKG_CONFIG_PATH=str(tmp_path),
               PKG_CONFIG_SYSTEM_INCLUDE_PATH=str(include), CPLUS_INCLUDE_PATH=str(include))
    flags = subprocess.check_output(['pkg-config', '--cflags-only-I', 'ndnsd'], env=env, text=True)
    assert not flags.strip()
    actual = subprocess.check_output(['pkg-config', '--variable=includedir', 'ndnsd'], env=env, text=True)
    assert actual.strip() == str(include)
    text = TEMPLATE.read_text()
    assert 'pkg-config --variable=includedir ndnsd' in text
    assert 'pkg-config --cflags-only-I ndnsd' not in text

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
              "@NATIVE_DIGEST@": "sha256:" + "b" * 64,
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
    assert "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib" in text
    assert "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib:/opt/ndn-base/lib" not in text
    assert "export NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-stage" in text


def test_explicit_core_directory_rejects_dependency_only_base(tmp_path):
    path = render(tmp_path)
    text = path.read_text().replace(
        "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib",
        "NDNSF_LIBRARY_DIR=/opt/ndnsf-stage/lib:/opt/ndn-base/lib", 1)
    path.write_text(text)
    with pytest.raises(boundary.Spec170BuildBoundaryError,
                       match="PYTHON_STAGE_LIBRARY_CLOSURE_MISMATCH"):
        boundary.validate_definition(path)


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


def test_builder_consumes_certified_base_without_external_builds(tmp_path):
    text = render(tmp_path).read_text()
    stages = {s.name: s for s in boundary._parse_stages(text)}
    builder = stages['builder'].sections['post']
    assert '/opt/ndn-base/manifest/dependency-sdk.py verify' in builder
    assert '"/opt/ndn-base/lib/$library" "/opt/ndnsf-stage/lib/$library"' in builder
    assert 'BASE_DEPENDENCY_IDENTITY_MISMATCH' in builder
    assert 'BASE_NATIVE_INPUT_IDENTITY_MISMATCH' in builder
    for forbidden in ['apt-get', '/src/nac-abe -B', '/src/onnx -B',
                      'cd /src/ndn-svs', 'cd /src/ndn-sd', 'install.sh',
                      '/build-input/wheels']:
        assert forbidden not in text
    assert 'BASE_RUNTIME_PACKAGE_CHANGED' in stages['final'].sections['post']
    assert '--ndn-svs-source-tree=/opt/ndn-base/sdk/sources/ndn-svs' in builder
    assert '--ndn-svs-build-tree=/opt/ndn-base/sdk/sources/ndn-svs/build' in builder
    for line in builder.splitlines():
        if 'export LD_LIBRARY_PATH=' in line:
            assert '/opt/ndn-base/sdk' not in line
    assert 'python3.10-dev' not in builder
    assert "Path(sysconfig.get_paths()['include'], 'Python.h').is_file()" in builder
    assert '-Wl,-rpath,/opt/ndnsf-di/current/lib' not in builder


def test_repository_tokenizer_build_uses_base_vendor_identity(tmp_path):
    text = render(tmp_path).read_text()
    assert '@BUNDLE@' not in text
    assert 'native/native-inputs.json /build-input/native/native-inputs.json' in text
    assert '--onnx-prefix=/opt/onnx' in text
    assert 'export NDNSF_CARGO_HOME=/opt/cargo-home' in text
    assert 'export NDNSF_RUST_PREFIX=/opt/rust' in text
    assert 'export NDNSF_TOKENIZER_BRIDGE_TARGET=/opt/ndnsf-build-work/tokenizer' in text
    assert "manifest['crate'][name]" in text


def test_builder_scratch_never_cleans_host_tmp_paths(tmp_path):
    text = render(tmp_path).read_text()
    builder = {s.name: s for s in boundary._parse_stages(text)}['builder'].sections['post']
    assert builder.index('export TMPDIR=/opt/ndnsf-build-work/tmp') < builder.index('dependency-sdk.py verify')
    assert 'unset TMPDIR' in builder
    for line in builder.splitlines():
        if line.strip().startswith('rm '):
            assert not re.search(r'(^|\s)/tmp(?:/|\s|$)', line)
    assert '/tmp/ndnsf-build-python' not in builder
    assert '/tmp/ndnsf-pc' not in builder


@pytest.mark.parametrize('mutation', ['none', 'base', 'crate'])
def test_native_input_preflight_rejects_base_or_crate_drift(tmp_path, mutation):
    # Execute the template's actual preflight with isolated filesystem inputs.
    native = tmp_path / 'native'
    native.mkdir()
    crate = tmp_path / 'crate'
    crate.mkdir()
    files = {}
    for name in ['Cargo.toml', 'Cargo.lock']:
        (crate / name).write_text(name)
        files[name] = 'sha256:' + hashlib.sha256((crate / name).read_bytes()).hexdigest()
    manifest = {'schema': 'ndnsf-native-build-inputs-v1',
                'onnxRevision': 'b8baa8446686496da4cc8fda09f2b6fe65c2a02c',
                'rustVersion': '1.90.0', 'crate': files}
    manifest_path = native / 'native-inputs.json'
    manifest_path.write_text(json.dumps(manifest))
    sha = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    sdk = tmp_path / 'dependency-sdk.json'
    sdk.write_text(json.dumps({'inputs': {'files': {
        'native/native-inputs.json': '0' * 64 if mutation == 'base' else sha}}}))
    if mutation == 'crate':
        (crate / 'Cargo.lock').write_text('different dependencies')
    snippets = re.findall(r"<<'PY'\n(.*?)\nPY(?:\n|$)", TEMPLATE.read_text(), re.S)
    snippet = next(s for s in snippets if 'BASE_NATIVE_INPUT_IDENTITY_MISMATCH' in s)
    snippet = snippet.replace('@NATIVE_DIGEST@', 'sha256:' + sha)
    snippet = snippet.replace('/build-input/native', str(native))
    snippet = snippet.replace('/opt/ndn-base/manifest/dependency-sdk.json', str(sdk))
    snippet = snippet.replace('/src/ndnsf/NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge', str(crate))
    if mutation == 'none':
        exec(compile(snippet, 'native-preflight', 'exec'), {})
    else:
        with pytest.raises(AssertionError):
            exec(compile(snippet, 'native-preflight', 'exec'), {})
