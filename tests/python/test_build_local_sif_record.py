import hashlib
import json
import os
import subprocess
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_LOCAL_SIF = (
    ROOT
    / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    / "build-local-sif.sh"
)
HOST_GATE = ROOT / "results/spec175/g3/spec175-g3-post-repo-readiness-r5-20260827.json"


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
    for path in /opt/venv/lib/python3.10/site-packages/torch* \\
                /opt/venv/lib/python3.10/site-packages/transformers* \\
                /opt/venv/lib/python3.10/site-packages/functorch*; do
        if [ -e "$path" ]; then rm -rf "$path"; fi
    done
    find /opt/venv/lib/python3.10/site-packages/ndnsf -name '_ndnsf*.so'
    sha256sum -c /opt/ndnsf-di/current/manifest/container-native-build.json

%labels
    org.ndnsf.di.build-boundary container-runtime-in-sif
    org.ndnsf.di.native-build-manifest /opt/ndnsf-di/current/manifest/container-native-build.json
{extra_labels}"""


def write_source_seal(root: Path, *, source_revision: str = "test-revision") -> Path:
    source = root / "sealed-source.txt"
    source.write_text("sealed source\n", encoding="utf-8")
    archive = root / "workspace.tar"
    with tarfile.open(archive, "w") as stream:
        stream.add(source, arcname="sealed-source.txt")
        workload = ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"
        stream.add(workload, arcname="packaging/ndnsf-di-container/jobs/spec175/workload.json")
    workload = ROOT / "packaging/ndnsf-di-container/jobs/spec175/workload.json"
    row = [
        {
            "path": "sealed-source.txt",
            "bytes": source.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(source.read_bytes()).hexdigest(),
        },
        {
            "path": "packaging/ndnsf-di-container/jobs/spec175/workload.json",
            "bytes": workload.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(workload.read_bytes()).hexdigest(),
        },
    ]
    body = {
        "schemaVersion": "spec170-local-sif-source-v1",
        "sourceRevision": source_revision,
        "sourceMode": "sealed-current-worktree-files",
        "workspace": str(root),
        "archive": {
            "path": str(archive.resolve()),
            "bytes": archive.stat().st_size,
            "sha256": "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest(),
        },
        "fileCount": len(row),
        "files": row,
        "compiledPayloadCount": 0,
    }
    body["sealDigest"] = "sha256:" + hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    seal = root / "source-seal.json"
    seal.write_text(json.dumps(body, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")
    return seal


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
        "\"org.ndnsf.di.source-seal\":\"SOURCE_SEAL_PLACEHOLDER\","
        "\"org.ndnsf.di.native-build-manifest\":\"/opt/ndnsf-di/current/manifest/container-native-build.json\"}}}}'; exit 0; fi\n"
        "if [ \"$1\" = exec ]; then "
        "echo '{\"status\":\"PASS\",\"python\":\"3.10.18\",\"extension\":\"/opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so\",\"ldd\":{},\"replay\":{\"status\":\"PASS\",\"paths\":{},\"commands\":{\"mn\":\"/usr/bin/mn\",\"ovsVswitchd\":\"/usr/sbin/ovs-vswitchd\",\"ovsVsctl\":\"/usr/bin/ovs-vsctl\",\"ip\":\"/usr/sbin/ip\"},\"imports\":{\"ndn\":\"ndn\",\"minindn\":\"minindn\",\"mininet\":\"mininet\",\"py_repoclient\":\"py_repoclient\"}}}'; exit 0; fi\n"
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
    source_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
    ).strip()
    source_seal = write_source_seal(tmp_path, source_revision=source_revision)
    seal_digest = json.loads(source_seal.read_text(encoding="utf-8"))["sealDigest"]
    apptainer.write_text(
        apptainer.read_text(encoding="utf-8").replace(
            "SOURCE_SEAL_PLACEHOLDER", seal_digest),
        encoding="utf-8",
    )
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
            "--host-gate-manifest",
            str(HOST_GATE),
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
    assert body["spec175Preflight"]["status"] == "PASS"
    assert body["spec175Preflight"]["sif"]["runtime"]["status"] == "PASS"
    assert body["spec175Preflight"]["workload"]["model"]["revision"] == (
        "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9")


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
    source_seal = write_source_seal(tmp_path)
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
            "--host-gate-manifest",
            str(HOST_GATE),
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


def test_final_definition_must_remove_functorch_residue(tmp_path):
    """Do not rebuild a candidate that the Spec175 runtime probe will reject."""
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

    base = tmp_path / "base.sif"
    base.write_bytes(b"qualified-base-sif")
    source = tmp_path / "source.tar"
    source.write_bytes(b"sealed source")
    definition = tmp_path / "candidate.def"
    definition.write_text(
        valid_definition(base, source).replace(
            "                /opt/venv/lib/python3.10/site-packages/functorch*; do\n",
            "; do\n",
        ),
        encoding="utf-8",
    )
    source_seal = write_source_seal(tmp_path)
    result = subprocess.run(
        [
            str(BUILD_LOCAL_SIF), "--definition", str(definition),
            "--sif", str(tmp_path / "runtime.sif"),
            "--record", str(tmp_path / "build-record.json"),
            "--source-seal", str(source_seal), "--host-gate-manifest", str(HOST_GATE),
            "--apptainer", str(apptainer),
            "--expected-apptainer", "1.3.4-1.el9",
        ],
        cwd=ROOT, capture_output=True, text=True,
    )
    assert result.returncode == 4
    assert "WRONG_BUILD_BOUNDARY_FUNCTORCH_REMOVAL_MISSING" in result.stderr
    assert invocation_log.read_text(encoding="utf-8").splitlines() == ["version"]


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
    source_seal = write_source_seal(tmp_path)
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
            "--host-gate-manifest",
            str(HOST_GATE),
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


def test_stale_declared_source_seal_label_is_rejected_before_build(tmp_path):
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

    base = tmp_path / "base.sif"
    base.write_bytes(b"qualified-base-sif")
    source = tmp_path / "source.tar"
    source.write_bytes(b"sealed source")
    definition = tmp_path / "candidate.def"
    definition.write_text(
        valid_definition(
            base,
            source,
            "    org.ndnsf.di.source-seal sha256:stale\n",
        ),
        encoding="utf-8",
    )
    source_seal = write_source_seal(tmp_path)

    result = subprocess.run(
        [
            str(BUILD_LOCAL_SIF),
            "--definition", str(definition),
            "--sif", str(tmp_path / "runtime.sif"),
            "--record", str(tmp_path / "build-record.json"),
            "--source-seal", str(source_seal),
            "--host-gate-manifest", str(HOST_GATE),
            "--apptainer", str(apptainer),
            "--expected-apptainer", "1.3.4-1.el9",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 4
    assert "LOCAL_SIF_DEFINITION_SOURCE_SEAL_LABEL_MISMATCH" in result.stderr
    assert invocation_log.read_text(encoding="utf-8").splitlines() == ["version"]


def test_spec175_strict_host_source_identity_rejects_stale_g3(tmp_path):
    """A 30/30 result from an older runner must not unlock a new SIF build."""
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
    source_seal = write_source_seal(tmp_path)
    definition = tmp_path / "candidate.def"
    definition.write_text("Bootstrap: localimage\nFrom: /missing/base.sif\n", encoding="utf-8")
    result = subprocess.run(
        [
            str(BUILD_LOCAL_SIF), "--definition", str(definition),
            "--sif", str(tmp_path / "runtime.sif"),
            "--record", str(tmp_path / "build-record.json"),
            "--source-seal", str(source_seal),
            "--host-gate-manifest", str(HOST_GATE),
            "--strict-host-source-seal",
            "--apptainer", str(apptainer), "--expected-apptainer", "1.3.4-1.el9",
        ],
        cwd=ROOT, capture_output=True, text=True,
    )
    assert result.returncode == 4
    assert "LOCAL_SIF_HOST_GATE_SOURCE_SEAL_INVALID" in result.stderr
    assert invocation_log.read_text(encoding="utf-8").splitlines() == ["version"]
