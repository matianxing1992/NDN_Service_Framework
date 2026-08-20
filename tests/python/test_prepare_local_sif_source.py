from __future__ import annotations

import json
import subprocess
import sys
import tarfile
from pathlib import Path


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
    assert "NDNSF-DistributedRepo/wscript" in names
    assert "libndn-service-framework.pc.in" in names
    assert not any(name.endswith((".so", ".a", ".o", ".pyc")) for name in names)
    assert not any("/build/" in f"/{name}/" for name in names)
    assert not any("/__pycache__/" in f"/{name}/" for name in names)


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
    assert report["ndnSvsArchive"]["sha256"] == dependency["archive"]["sha256"]
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
