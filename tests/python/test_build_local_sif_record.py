import hashlib
import json
import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_LOCAL_SIF = (
    ROOT
    / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    / "build-local-sif.sh"
)


def valid_definition(base: Path, source: Path, extra_labels: str = "") -> str:
    return f"""Bootstrap: localimage
From: {base}
Stage: builder

%files
    {source} /build/source.tar

%post
    export NDNSF_CONTAINER_BUILD=1
    ./waf --targets=di-native-provider
    /opt/venv/bin/pip install ./pythonWrapper
    touch /opt/ndnsf-stage/manifest/container-configure-closure.json
    touch /opt/ndnsf-stage/manifest/container-native-build.json

Bootstrap: localimage
From: {base}
Stage: final

%files from builder
    /opt/ndnsf-stage/bin/di-native-provider /opt/ndnsf-di/current/bin/di-native-provider
    /opt/ndnsf-stage/lib/libndn-service-framework.so.0.1.0 /opt/ndnsf-di/current/lib/libndn-service-framework.so.0.1.0
    /opt/ndnsf-stage/python /opt/venv/lib/python3.10/site-packages
    /opt/ndnsf-stage/manifest/container-native-build.json /opt/ndnsf-di/current/manifest/container-native-build.json

%post
    export NDNSF_REPLACE_STALE_NATIVE=1
    rm -f /opt/ndnsf-di/current/bin/di-native-provider
    rm -f /opt/ndnsf-di/current/lib/libndn-service-framework.so*
    rm -f /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf*.so
    find /opt/venv/lib/python3.10/site-packages/ndnsf -name '_ndnsf*.so'
    sha256sum -c /opt/ndnsf-di/current/manifest/container-native-build.json

%labels
    org.ndnsf.di.build-boundary container-runtime-in-sif
    org.ndnsf.di.native-build-manifest /opt/ndnsf-di/current/manifest/container-native-build.json
{extra_labels}"""


def test_localimage_base_is_hash_bound_in_build_record(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    apptainer = tools / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = version ]; then echo 1.3.4; exit 0; fi\n"
        "if [ \"$1\" = build ]; then shift; "
        "  [ \"$1\" = --force ] && shift; cp \"$2\" \"$1\"; exit 0; fi\n"
        "if [ \"$1\" = inspect ]; then "
        "  echo '{\"data\":{\"attributes\":{\"labels\":{"
        "\"org.ndnsf.di.build-boundary\":\"container-runtime-in-sif\","
        "\"org.ndnsf.di.native-build-manifest\":\"/opt/ndnsf-di/current/manifest/container-native-build.json\"}}}}'; exit 0; fi\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)

    base = tmp_path / "base.sif"
    base.write_bytes(b"qualified-base-sif")
    source = tmp_path / "source.tar"
    source.write_bytes(b"sealed source")
    definition = tmp_path / "candidate.def"
    definition.write_text(
        valid_definition(base, source),
        encoding="utf-8",
    )
    source_seal = tmp_path / "source-seal.json"
    source_seal.write_text('{"status":"PASS"}\n', encoding="utf-8")
    candidate = tmp_path / "runtime.sif"
    record = tmp_path / "build-record.json"

    env = os.environ.copy()
    env["PATH"] = f"{tools}:{env['PATH']}"
    subprocess.run(
        [
            str(BUILD_LOCAL_SIF),
            "--definition",
            str(definition),
            "--sif",
            str(candidate),
            "--record",
            str(record),
            "--source-seal",
            str(source_seal),
            "--apptainer",
            str(apptainer),
            "--expected-apptainer",
            "1.3.4-1.el9",
        ],
        cwd=ROOT,
        env=env,
        check=True,
        capture_output=True,
        text=True,
    )

    body = json.loads(record.read_text(encoding="utf-8"))
    expected_sha = hashlib.sha256(base.read_bytes()).hexdigest()
    assert body["buildInput"]["baseSif"] == {
        "path": str(base.resolve()),
        "sha256": f"sha256:{expected_sha}",
        "bytes": base.stat().st_size,
    }
    assert body["hostRole"] == "apptainer-driver-only"
    assert body["containerNativeBuild"]["status"] == "PASS"


def test_r13_style_host_binaries_are_rejected_before_apptainer_build(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    invocation_log = tmp_path / "apptainer-invocations.log"
    apptainer = tools / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        f"printf '%s\\n' \"$*\" >> {invocation_log}\n"
        "if [ \"$1\" = version ]; then echo 1.3.4; exit 0; fi\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)

    definition = tmp_path / "r13.def"
    definition.write_text(
        "Bootstrap: localimage\n"
        f"From: {tmp_path / 'base.sif'}\n\n"
        "%files\n"
        f"    {tmp_path / 'host-build' / 'di-native-provider'} "
        "/opt/ndnsf-di/current/bin/di-native-provider\n"
        f"    {tmp_path / 'host-build' / 'ndnsf'} "
        "/opt/venv/lib/python3.10/site-packages/ndnsf\n",
        encoding="utf-8",
    )
    source_seal = tmp_path / "source-seal.json"
    source_seal.write_text('{"status":"PASS"}\n', encoding="utf-8")
    candidate = tmp_path / "runtime.sif"
    record = tmp_path / "build-record.json"

    result = subprocess.run(
        [
            str(BUILD_LOCAL_SIF),
            "--definition",
            str(definition),
            "--sif",
            str(candidate),
            "--record",
            str(record),
            "--source-seal",
            str(source_seal),
            "--apptainer",
            str(apptainer),
            "--expected-apptainer",
            "1.3.4-1.el9",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 4
    assert "WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT" in result.stderr
    assert invocation_log.read_text(encoding="utf-8").splitlines() == ["version"]
    assert not candidate.exists()
    assert not record.exists()


def test_declared_release_label_mismatch_rejects_candidate(tmp_path):
    tools = tmp_path / "tools"
    tools.mkdir()
    apptainer = tools / "apptainer"
    apptainer.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = version ]; then echo 1.3.4; exit 0; fi\n"
        "if [ \"$1\" = build ]; then shift; "
        "  [ \"$1\" = --force ] && shift; cp \"$2\" \"$1\"; exit 0; fi\n"
        "if [ \"$1\" = inspect ]; then "
        "  echo '{\"data\":{\"attributes\":{\"labels\":{"
        "\"org.ndnsf.di.build-boundary\":\"container-runtime-in-sif\","
        "\"org.ndnsf.di.native-build-manifest\":\"/opt/ndnsf-di/current/manifest/container-native-build.json\","
        "\"org.ndnsf.di.release\":\"old\"}}}}'; "
        "  exit 0; fi\n"
        "exit 97\n",
        encoding="utf-8",
    )
    apptainer.chmod(0o755)

    base = tmp_path / "base.sif"
    base.write_bytes(b"qualified-base-sif")
    source = tmp_path / "source.tar"
    source.write_bytes(b"sealed source")
    definition = tmp_path / "candidate.def"
    definition.write_text(
        valid_definition(base, source, "    org.ndnsf.di.release new\n"),
        encoding="utf-8",
    )
    source_seal = tmp_path / "source-seal.json"
    source_seal.write_text('{"status":"PASS"}\n', encoding="utf-8")
    candidate = tmp_path / "runtime.sif"
    record = tmp_path / "build-record.json"
    env = os.environ.copy()
    env["PATH"] = f"{tools}:{env['PATH']}"

    result = subprocess.run(
        [
            str(BUILD_LOCAL_SIF),
            "--definition",
            str(definition),
            "--sif",
            str(candidate),
            "--record",
            str(record),
            "--source-seal",
            str(source_seal),
            "--apptainer",
            str(apptainer),
            "--expected-apptainer",
            "1.3.4-1.el9",
        ],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 4
    assert "LOCAL_SIF_LABEL_MISMATCH" in result.stderr
    assert not candidate.exists()
    assert not record.exists()
