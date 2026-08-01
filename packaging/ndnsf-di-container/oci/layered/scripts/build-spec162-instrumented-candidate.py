#!/usr/bin/env python3
"""Build the small Spec 162 measurement overlay and its exact source bundle."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[5]
LAYERED = ROOT / "packaging/ndnsf-di-container/oci/layered"
DOCKERFILE = LAYERED / "Dockerfile.spec162-instrumentation"
BASE_TAG = "ndnsf-di:spec162-qwen36-t009-20260729a"
BASE_IMAGE_ID = (
    "sha256:eaf7c064c0fc5011f6a6e3502903d156"
    "36694e7d8d523591d80c366b5d1ed819"
)
PROVIDER = (
    ROOT
    / "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py"
)
REPO_CLIENT_ROOT = (
    ROOT / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient"
)
LLM_ROOT = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"
)
JOBS_ROOT = ROOT / "specs/162-itiger-qwen36-generation/jobs"
HARNESS_ROOT = (
    ROOT / "specs/160-itiger-multinode-qwen-collaboration/jobs"
)


class BuildError(RuntimeError):
    pass


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(
    command: list[str],
    *,
    log: Path | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess:
    if log is not None:
        with log.open("x", encoding="utf-8") as stream:
            return subprocess.run(
                command,
                cwd=ROOT,
                stdout=stream,
                stderr=subprocess.STDOUT,
                text=True,
                check=True,
            )
    return subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True,
        check=True,
    )


def image_record(tag: str) -> dict:
    result = run(
        ["docker", "image", "inspect", tag],
        capture=True,
    )
    value = json.loads(result.stdout)[0]
    return {
        "tag": tag,
        "imageId": value["Id"],
        "sizeBytes": int(value["Size"]),
        "repoDigests": list(value.get("RepoDigests") or ()),
        "labels": dict(value.get("Config", {}).get("Labels") or {}),
    }


def selected_sources() -> dict[str, Path]:
    values: dict[str, Path] = {
        "image/ndnsf_distributed_inference/provider.py": PROVIDER,
        "build/Dockerfile.spec162-instrumentation": DOCKERFILE,
        "build/build-spec162-instrumented-candidate.py": Path(__file__).resolve(),
        "build/qwen36-overlay.lock.json": (
            LAYERED / "locks/qwen36-overlay.lock.json"
        ),
    }
    for name in ("orchestration.py", "persistence.py", "service_names.py"):
        values[f"image/py_repoclient/{name}"] = REPO_CLIENT_ROOT / name
    for path in sorted(LLM_ROOT.glob("*.py")):
        values[f"llm_pipeline/{path.name}"] = path
    for path in sorted(JOBS_ROOT.iterdir()):
        if path.is_file() and (
            path.suffix in {".py", ".sh", ".sbatch", ".in"}
            or path.name == "nfd.conf.in"
        ):
            values[f"jobs/{path.name}"] = path
    for name in (
        "local-docker-operation-status-inner.sh",
        "local-docker-operation-status-policy.yaml",
    ):
        values[f"harness/{name}"] = HARNESS_ROOT / name
    return values


def source_seal(sources: dict[str, Path]) -> dict:
    rows = {
        target: {
            "sourcePath": str(path.relative_to(ROOT)),
            "sha256": "sha256:" + sha256_file(path),
            "bytes": path.stat().st_size,
        }
        for target, path in sorted(sources.items())
    }
    body = {
        "schemaVersion": "spec162-instrumentation-source-seal-v1",
        "createdAt": "1970-01-01T00:00:00Z",
        "baseImageId": BASE_IMAGE_ID,
        "sources": rows,
    }
    body["sealDigest"] = "sha256:" + sha256_bytes(
        json.dumps(
            body,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    )
    return body


def copy_source_bundle(
    output: Path,
    sources: dict[str, Path],
    seal: dict,
) -> Path:
    bundle = output / "source-bundle"
    bundle.mkdir()
    for target, source in sorted(sources.items()):
        if target.startswith("image/"):
            continue
        destination = bundle / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o500 if destination.suffix in {".sh", ".py"} else 0o400)
    seal_path = bundle / "source-seal.json"
    seal_path.write_text(
        json.dumps(seal, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    seal_path.chmod(0o400)
    for directory, subdirs, _files in os.walk(bundle, topdown=False):
        Path(directory).chmod(0o500)
        for subdir in subdirs:
            (Path(directory) / subdir).chmod(0o500)
    return bundle


def build(*, output: Path, tag: str) -> dict:
    if output.exists() and any(output.iterdir()):
        raise BuildError("OUTPUT_DIRECTORY_NOT_EMPTY")
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "build-manifest.json"
    started = time.monotonic()
    manifest: dict = {
        "schemaVersion": "spec162-instrumented-candidate-build-v1",
        "status": "RUNNING",
        "startedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "baseTag": BASE_TAG,
        "baseImageId": BASE_IMAGE_ID,
        "targetTag": tag,
        "modelsIncluded": False,
        "published": False,
    }
    try:
        base = image_record(BASE_TAG)
        if base["imageId"] != BASE_IMAGE_ID:
            raise BuildError("BASE_IMAGE_ID_MISMATCH")
        if base["repoDigests"]:
            raise BuildError("BASE_IMAGE_UNEXPECTEDLY_PUBLISHED")

        sources = selected_sources()
        if any(not path.is_file() for path in sources.values()):
            raise BuildError("INSTRUMENTATION_SOURCE_MISSING")
        seal = source_seal(sources)
        seal_path = output / "instrumentation-source-seal.json"
        seal_path.write_text(
            json.dumps(seal, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        provider_sha = sha256_file(PROVIDER)
        repo_orchestration_sha = sha256_file(
            REPO_CLIENT_ROOT / "orchestration.py")
        repo_service_names_sha = sha256_file(
            REPO_CLIENT_ROOT / "service_names.py")
        repo_persistence_sha = sha256_file(
            REPO_CLIENT_ROOT / "persistence.py")

        with tempfile.TemporaryDirectory(
            prefix="spec162-instrumentation-context."
        ) as temporary:
            context = Path(temporary)
            shutil.copyfile(PROVIDER, context / "provider.py")
            repo_context = context / "py_repoclient"
            repo_context.mkdir()
            for name in (
                "orchestration.py", "persistence.py", "service_names.py"
            ):
                shutil.copyfile(
                    REPO_CLIENT_ROOT / name,
                    repo_context / name,
                )
            run(
                [
                    "docker", "buildx", "build",
                    "--load",
                    "--network=none",
                    "--file", str(DOCKERFILE),
                    "--build-context", f"instrumentation_seal={context}",
                    "--build-arg", f"BASE_IMAGE={BASE_TAG}",
                    "--build-arg", f"BASE_IMAGE_ID={BASE_IMAGE_ID}",
                    "--build-arg", f"PROVIDER_SHA256={provider_sha}",
                    "--build-arg",
                    f"REPO_ORCHESTRATION_SHA256={repo_orchestration_sha}",
                    "--build-arg",
                    f"REPO_SERVICE_NAMES_SHA256={repo_service_names_sha}",
                    "--build-arg",
                    f"REPO_PERSISTENCE_SHA256={repo_persistence_sha}",
                    "--build-arg",
                    f"INSTRUMENTATION_SEAL={seal['sealDigest']}",
                    "--tag", tag,
                    str(ROOT),
                ],
                log=output / "docker-build.log",
            )

        image = image_record(tag)
        labels = image["labels"]
        if labels.get(
            "org.ndnsf.di.instrumentation-parent-image-id"
        ) != BASE_IMAGE_ID:
            raise BuildError("INSTRUMENTATION_PARENT_LABEL_MISMATCH")
        if labels.get(
            "org.ndnsf.di.instrumentation-seal"
        ) != seal["sealDigest"]:
            raise BuildError("INSTRUMENTATION_SEAL_LABEL_MISMATCH")
        if image["sizeBytes"] - base["sizeBytes"] > 2 * 1024 * 1024:
            raise BuildError("INSTRUMENTATION_LAYER_UNEXPECTEDLY_LARGE")

        probe = run(
            [
                "docker", "run", "--rm",
                "--user", "0",
                "--env", "HOME=/tmp",
                "--entrypoint", "/opt/venv/bin/python",
                tag,
                "-c",
                (
                    "import inspect, json; "
                    "import ndnsf_distributed_inference.provider as p; "
                    "import py_repoclient.orchestration as o; "
                    "import py_repoclient.service_names as service_names; "
                    "provider_source=inspect.getsource(p); "
                    "assert 'NDNSF_DI_ACK_DECISION' in provider_source; "
                    "repo=inspect.getsource(o); "
                    "names=inspect.getsource(service_names); "
                    "assert 'RepoCapacityReservation' not in repo; "
                    "assert 'RESERVE_CAPACITY' not in names; "
                    "assert 'RELEASE_CAPACITY' not in names; "
                    "print(json.dumps({'status':'PASS','path':p.__file__},"
                    "sort_keys=True))"
                ),
            ],
            capture=True,
        )
        (output / "runtime-probe.json").write_text(
            probe.stdout,
            encoding="utf-8",
        )
        bundle = copy_source_bundle(output, sources, seal)
        manifest.update({
            "status": "PASS",
            "sourceSeal": seal["sealDigest"],
            "sourceCount": len(sources),
            "providerSha256": "sha256:" + provider_sha,
            "repoOrchestrationSha256":
                "sha256:" + repo_orchestration_sha,
            "repoServiceNamesSha256":
                "sha256:" + repo_service_names_sha,
            "repoPersistenceSha256":
                "sha256:" + repo_persistence_sha,
            "image": image,
            "sourceBundle": str(bundle),
        })
    except Exception as error:
        manifest["status"] = "FAIL"
        manifest["reasonCode"] = str(error)
        raise
    finally:
        manifest["finishedAt"] = dt.datetime.now(dt.timezone.utc).isoformat()
        manifest["durationSeconds"] = round(time.monotonic() - started, 3)
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    try:
        value = build(output=args.output, tag=args.tag)
    except Exception as error:
        print(str(error))
        return 4
    print(json.dumps(value, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
