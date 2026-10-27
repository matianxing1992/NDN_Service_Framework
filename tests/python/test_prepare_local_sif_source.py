from __future__ import annotations

import ast
import json
import os
import runpy
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = (
    ROOT / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
    / "prepare-local-sif-source.py"
)
VALIDATOR = SCRIPT.with_name("validate-local-sif-source.py")


def test_source_archive_excludes_host_binaries_and_build_output(tmp_path):
    output = tmp_path / "sealed"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--workspace",
            str(ROOT),
            "--output-dir",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    report = json.loads(result.stdout)
    assert report["status"] == "PASS"
    seal = json.loads((output / "source-seal.json").read_text(encoding="utf-8"))
    assert seal["compiledPayloadCount"] == 0
    with tarfile.open(output / "workspace.tar", "r") as archive:
        names = archive.getnames()
    assert "pythonWrapper/src/ndnsf/_ndnsf.cpp" in names
    assert "examples/App_ServiceController.cpp" in names
    assert "examples/DI_NativeArtifactAuthority.cpp" in names
    assert "NDNSF-DistributedRepo/wscript" in names
    assert "libndn-service-framework.pc.in" in names
    assert "packaging/ndnsf-di-container/jobs/spec175/workload.json" in names
    assert "scripts/build_spec175_workload.py" in names
    assert "scripts/run_spec180_case.py" in names
    assert "tests/fixtures/spec180/yolo26n/fixed-fixture.ppm" in names
    assert "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/reference.py" in names
    assert "NDNSF-DistributedInference/cpp/adapters/onnx/CudaDeviceIdentity.hpp" in names
    assert "tests/fixtures/spec180/fake-cuda-identity.cpp" not in names
    assert "tests/fixtures/spec180/native-evidence-probe.cpp" not in names
    assert "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py" in names
    assert "examples/trust-schema.conf" in names
    assert "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py" in names
    assert "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py" in names
    for required in (
        "install_ndnsf_stack.sh", "scripts/configure_dependencies.py", "scripts/host_profile.py",
        "scripts/build-safe.sh", "scripts/stack_sources.py", "packaging/host-dependencies.lock.json",
        "packaging/host-sdk-dependencies.lock.json", "patches/nac-abe/series.json",
        "packaging/ndnsf-di-container/lib/allocation_topology.py",
        "packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-container.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/stage-sif.sh",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/ops/deployment_config.py",
        "patches/openabe/series.json", "NDNSF-DistributedRepo/apps/DistributedRepoServer.cpp",
        "tools/ndn-svs/publication-latency.patch",
        "ndnsf-di/api.hpp", "ndnsf-di/provider.hpp",
        "NDNSF-UAV-APP/tracking/tracking_worker.py", "examples/DI_NativeRequester.cpp",
    ):
        assert required in names
    assert not any(name.startswith("specs/") for name in names)
    assert not any(name.endswith((".so", ".a", ".o", ".pyc")) for name in names)
    assert not any("/build/" in f"/{name}/" for name in names)
    assert not any("/__pycache__/" in f"/{name}/" for name in names)
    production_roles = {"App_ServiceController", "DI_NativeRequester", "DI_NativeArtifactAuthority",
                        "di-native-provider", "di-native-assembly-worker", "ndnsf-di-device-query", "NDNSF_RepoPacketServer",
                        "DistributedRepoServer", "UavDroneApp", "UavGroundStationApp",
                        "UavTrackingNode", "UavSensorStreamNode", "UavTrackingGroundStation"}
    observed_roles = set()
    for build_file in (ROOT / "examples/wscript", ROOT / "NDNSF-DistributedRepo/wscript"):
        for call in ast.walk(ast.parse(build_file.read_text())):
            if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Attribute):
                continue
            if call.func.attr != "program":
                continue
            keywords = {item.arg: item.value for item in call.keywords}
            name = keywords.get("name")
            if not isinstance(name, ast.Constant):
                continue
            if name.value in production_roles:
                observed_roles.add(name.value)
            elif build_file.parent.name != "NDNSF-DistributedRepo":
                continue
            for literal in ast.walk(keywords["source"]):
                if (not isinstance(literal, ast.Constant) or not isinstance(literal.value, str) or
                        not literal.value.endswith(".cpp") or "*" in literal.value):
                    continue
                candidates = (build_file.parent / literal.value, ROOT / literal.value)
                source = next(p.resolve() for p in candidates if p.is_file())
                assert source.relative_to(ROOT).as_posix() in names
    assert observed_roles == production_roles


def test_relocated_export_runs_actual_installer_and_locked_patch_reader(tmp_path):
    output = tmp_path / "sealed"
    subprocess.run([sys.executable, str(SCRIPT), "--workspace", str(ROOT),
                    "--output-dir", str(output)], check=True, capture_output=True, text=True)
    subprocess.run([sys.executable, str(VALIDATOR), "--source-seal", str(output / "source-seal.json")],
                   check=True, capture_output=True, text=True)
    extracted = tmp_path / "extracted"
    extracted.mkdir()
    with tarfile.open(output / "workspace.tar") as archive:
        archive.extractall(extracted)
    moved = tmp_path / "relocated"
    extracted.rename(moved)
    assert not (moved / ".git").exists()
    assert not (moved / "specs").exists()
    env = dict(os.environ, PYTHONNOUSERSITE="1", PYTHONUSERBASE=str(tmp_path / "empty-user"))
    env.pop("PYTHONPATH", None)
    env.pop("PYTHONHOME", None)
    result = subprocess.run(["bash", str(moved / "install_ndnsf_stack.sh"), "--source", "--plan",
                             "--components", "di", "--no-system-packages"], cwd=moved, env=env,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert "NDNSF components: di (core,repo,di)" in result.stdout
    assert "Offline plan only" in result.stdout
    assert str(ROOT) not in result.stdout + result.stderr

    # load_lock validates every locked patch descriptor and actual patch bytes.
    module = runpy.run_path(str(moved / "scripts/stack_sources.py"))
    lock = module["load_lock"](moved / "packaging/host-dependencies.lock.json")
    assert set(lock["sources"]) == {"ndn-cxx", "ndn-svs", "NDNSD", "NAC-ABE", "openabe"}

    # Required current input absence is rejected before a partial archive.
    select = runpy.run_path(str(SCRIPT))["selected_files"]
    original = json.loads((output / "source-seal.json").read_text())
    assert {path.as_posix() for path in select(moved)} == {
        row["path"] for row in original["files"]}
    for relative in ("scripts/configure_dependencies.py", "tools/ndn-svs/publication-latency.patch",
                     "ndnsf-di/api.hpp", "ndnsf-di/provider.hpp",
                     "packaging/ndnsf-di-container/lib/allocation_topology.py",
                     "packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py",
                     "NDNSF-DistributedInference/ndnsf_distributed_inference/ops/deployment_config.py"):
        required = moved / relative
        content = required.read_bytes()
        required.unlink()
        with pytest.raises(SystemExit, match="LOCAL_SIF_SOURCE_MISSING:" + relative):
            select(moved)
        required.write_bytes(content)


def test_export_filters_generated_rust_and_unrelated_uav_data(tmp_path):
    select = runpy.run_path(str(SCRIPT))["selected_files"]
    select.__globals__["FILES"] = ("cpp",)
    (tmp_path / "wscript").write_text("")
    for name in ("cpp/backend.cpp", "cpp/target/debug/generated.rs", "cpp/libbackend.rlib",
                 "NDNSF-UAV-APP/tools/app.cpp", "NDNSF-UAV-APP/tracking/worker.py",
                 "NDNSF-UAV-APP/results/model.onnx", "NDNSF-UAV-APP/identity/private.key",
                 "NDNSF-UAV-APP/build/generated.cpp", "examples/app.cpp", "examples/app.log"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    paths = {p.as_posix() for p in select(tmp_path)}
    assert paths == {"wscript", "cpp/backend.cpp", "NDNSF-UAV-APP/tools/app.cpp",
                     "NDNSF-UAV-APP/tracking/worker.py", "examples/app.cpp"}



def test_source_sealer_does_not_define_host_substrate_dependency_archives():
    text = SCRIPT.read_text(encoding="utf-8")
    for option in ("--mini-ndn-workspace", "--nlsr-workspace",
                   "--ndn-tools-workspace", "--infoedit-workspace"):
        assert option not in text
    for dependency in ("miniNdn", "nlsr", "ndnTools", "infoedit"):
        assert f'dependency_reports["{dependency}"]' not in text


def test_source_archive_is_deterministic(tmp_path):
    first = tmp_path / "first"
    second = tmp_path / "second"
    for output in (first, second):
        subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--workspace",
                str(ROOT),
                "--output-dir",
                str(output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
    left = json.loads((first / "source-seal.json").read_text(encoding="utf-8"))
    right = json.loads((second / "source-seal.json").read_text(encoding="utf-8"))
    assert left["archive"]["sha256"] == right["archive"]["sha256"]
    assert left["files"] == right["files"]
    assert left["sealDigestBasis"] == "path-independent-content-v1"
    assert left["sealDigest"] == right["sealDigest"]


def test_source_archive_seals_ndn_svs_source_without_host_binaries(tmp_path):
    ndn_svs = tmp_path / "ndn-svs"
    for relative, content in {
        "waf": "#!/usr/bin/env python3\n",
        "wscript": "def build(bld):\n    pass\n",
        "VERSION.info": "0.1.0\n",
        "libndn-svs.pc.in": "Name: libndn-svs\n",
        ".waf-tools/boost.py": "# tool\n",
        "ndn-svs/svspubsub.hpp": "#pragma once\n",
        "ndn-svs/svspubsub.cpp": "// source\n",
        "build/libndn-svs.so": "host binary\n",
    }.items():
        path = ndn_svs / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=ndn_svs, check=True)
    subprocess.run(["git", "add", "."], cwd=ndn_svs, check=True)
    subprocess.run(
        ["git", "-c", "user.name=Spec170 Test", "-c",
         "user.email=spec170@example.invalid", "commit", "-qm", "fixture"],
        cwd=ndn_svs, check=True)

    output = tmp_path / "sealed"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--workspace",
            str(ROOT),
            "--ndn-svs-workspace",
            str(ndn_svs),
            "--output-dir",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    report = json.loads(result.stdout)
    seal = json.loads((output / "source-seal.json").read_text(encoding="utf-8"))
    dependency = seal["dependencies"]["ndnSvs"]
    assert report["dependencyArchives"]["ndnSvs"]["sha256"] == dependency["archive"]["sha256"]
    assert dependency["compiledPayloadCount"] == 0
    with tarfile.open(output / "ndn-svs.tar", "r") as archive:
        names = archive.getnames()
    assert "ndn-svs/svspubsub.hpp" in names
    assert "ndn-svs/svspubsub.cpp" in names
    assert "build/libndn-svs.so" not in names
    assert not any(name.endswith((".so", ".a", ".o", ".pyc")) for name in names)

def test_source_seal_validator_accepts_exact_archives(tmp_path):
    output = tmp_path / "sealed"
    subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--workspace",
            str(ROOT),
            "--output-dir",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), "--source-seal",
         str(output / "source-seal.json")],
        check=True,
        capture_output=True,
        text=True,
    )
    assert json.loads(result.stdout)["status"] == "PASS"


def test_source_seal_validator_rejects_tampered_archive(tmp_path):
    output = tmp_path / "sealed"
    subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--workspace",
            str(ROOT),
            "--output-dir",
            str(output),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    with (output / "workspace.tar").open("ab") as stream:
        stream.write(b"tampered")
    result = subprocess.run(
        [sys.executable, str(VALIDATOR), "--source-seal",
         str(output / "source-seal.json")],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 4
    assert "LOCAL_SIF_SOURCE_ARCHIVE_SIZE_MISMATCH:workspace" in result.stderr
