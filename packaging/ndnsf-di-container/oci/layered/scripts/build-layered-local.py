#!/usr/bin/env python3
"""Build Spec 158 ML, stable NDN, and mutable App OCI products."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import time


class BuildError(RuntimeError):
    pass


TRANSIENT_BUILD_ERRORS = re.compile(
    r"tls: bad record MAC|unexpected EOF|connection reset|i/o timeout|"
    r"TLS handshake timeout|503 Service Unavailable|429 Too Many Requests|"
    r"Hash Sum mismatch",
    re.IGNORECASE,
)


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def token(*values: str) -> str:
    return hashlib.sha256("\0".join(values).encode()).hexdigest()[:12]


def run_capture(command: list[str]) -> str:
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    if result.returncode:
        raise BuildError(
            f"COMMAND_FAILED:{command[0]}:{(result.stderr or result.stdout).strip()}"
        )
    return result.stdout.strip()


def image_record(tag: str) -> dict[str, object]:
    value = json.loads(run_capture(["docker", "image", "inspect", tag]))[0]
    return {
        "tag": tag,
        "imageId": value["Id"],
        "repoDigests": value.get("RepoDigests") or [],
        "sizeBytes": value["Size"],
        "created": value["Created"],
        "labels": value.get("Config", {}).get("Labels") or {},
    }


def existing(tag: str) -> bool:
    return subprocess.run(
        ["docker", "image", "inspect", tag],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    ).returncode == 0


def build(
    *,
    workspace: Path,
    dockerfile: Path,
    target: str,
    tag: str,
    args: dict[str, str],
    contexts: dict[str, Path],
    log: Path,
    expected_labels: dict[str, str],
    dry_run: bool,
) -> dict[str, object]:
    if existing(tag):
        record = image_record(tag)
        for key, value in expected_labels.items():
            if record["labels"].get(key) != value:
                raise BuildError(f"WRITE_ONCE_TAG_COLLISION:{tag}:{key}")
        record.update({"action": "reused", "durationSeconds": 0.0})
        return record
    command = [
        "docker", "buildx", "build", "--load", "--progress=plain",
        "--platform", "linux/amd64", "--target", target,
        "--file", str(dockerfile), "--tag", tag,
    ]
    for name, value in sorted(args.items()):
        command.extend(["--build-arg", f"{name}={value}"])
    for name, path in sorted(contexts.items()):
        command.extend(["--build-context", f"{name}={path}"])
    command.append(str(workspace))
    if dry_run:
        log.write_text(" ".join(command) + "\n")
        return {
            "tag": tag,
            "imageId": "dry-run:" + token(tag),
            "action": "dry-run",
            "command": command,
        }
    started = time.monotonic()
    attempts = 0
    while True:
        attempts += 1
        tail: list[str] = []
        with log.open("a" if attempts > 1 else "w", encoding="utf-8") as stream:
            stream.write(
                f"\n# attempt {attempts}\n$ " + " ".join(command) + "\n"
            )
            stream.flush()
            process = subprocess.Popen(
                command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
            )
            assert process.stdout is not None
            for line in process.stdout:
                stream.write(line)
                stream.flush()
                tail.append(line)
                if len(tail) > 200:
                    tail.pop(0)
                print(line, end="")
            return_code = process.wait()
        if return_code == 0:
            break
        detail = "".join(tail)
        if attempts >= 3 or TRANSIENT_BUILD_ERRORS.search(detail) is None:
            raise BuildError(
                f"DOCKER_BUILD_FAILED:{target}:exit={return_code}:"
                f"attempts={attempts}:log={log}"
            )
        delay = attempts * 5
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"# transient transport failure; retrying in {delay}s\n")
        time.sleep(delay)
    record = image_record(tag)
    for key, value in expected_labels.items():
        if record["labels"].get(key) != value:
            raise BuildError(f"BUILT_IMAGE_LABEL_MISMATCH:{tag}:{key}")
    record.update({
        "action": "built",
        "attempts": attempts,
        "durationSeconds": round(time.monotonic() - started, 3),
        "log": str(log),
    })
    return record


def assert_binding(tag: str, expected_id: str) -> None:
    observed = image_record(tag)["imageId"]
    if observed != expected_id:
        raise BuildError(f"PARENT_TAG_DRIFT:{tag}:{observed}:{expected_id}")


def scan_image(tag: str) -> dict[str, object]:
    container = run_capture([
        "docker", "create", "--entrypoint", "/bin/true", tag
    ])
    banned: list[str] = []
    banned_count = 0
    try:
        process = subprocess.Popen(
            ["docker", "export", container], stdout=subprocess.PIPE
        )
        assert process.stdout is not None
        with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                path = PurePosixPath(member.name)
                lower = member.name.lower()
                is_public_ca = (
                    member.name.startswith("etc/ssl/certs/")
                    or member.name.startswith("usr/share/ca-certificates/")
                    or (
                        "/certifi/" in member.name
                        and lower.endswith("cacert.pem")
                    )
                    or (
                        member.name.startswith("usr/share/gnupg/")
                        and lower.endswith(".pem")
                    )
                )
                is_banned = (
                    bool(path.parts and path.parts[0] == "src")
                    or any(
                        part in {".git", "identities", "secrets"}
                        for part in path.parts
                    )
                    or (
                        lower.endswith((
                            ".onnx", ".onnx_data", ".pt", ".safetensors",
                            ".gguf", ".ckpt", ".key", ".pem", ".p12",
                        ))
                        and not is_public_ca
                    )
                )
                if is_banned:
                    banned_count += 1
                    if len(banned) < 20:
                        banned.append(member.name)
        if process.wait():
            raise BuildError("IMAGE_EXPORT_FAILED")
    finally:
        subprocess.run(
            ["docker", "container", "rm", "-f", container],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    if banned:
        raise BuildError(
            f"FINAL_IMAGE_BANNED_CONTENT:count={banned_count}:"
            + ",".join(banned)
        )
    return {"status": "PASS", "bannedPathCount": banned_count}


def static_probe(tag: str, output: Path) -> dict[str, object]:
    command = [
        "docker", "run", "--rm", tag, "exec",
        "/usr/local/bin/ndnsf-di-probe-runtime", "--mode", "static",
    ]
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    output.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise BuildError(f"FINAL_STATIC_PROBE_FAILED:{output}")
    value = json.loads(result.stdout)
    if value.get("status") != "PASS":
        raise BuildError("FINAL_STATIC_PROBE_REPORTED_FAIL")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=("ml", "ndn", "app", "all"), required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--app-build-id")
    parser.add_argument("--ndn-svs", default="/home/tianxing/NDN/ndn-svs")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 4:
        raise SystemExit("BUILD_JOBS_OUT_OF_RANGE")
    workspace = Path(__file__).resolve().parents[5]
    output = Path(args.output).resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("BUILD_OUTPUT_NOT_EMPTY")
    output.mkdir(parents=True, exist_ok=True)
    layered = workspace / "packaging/ndnsf-di-container/oci/layered"
    locks = layered / "locks"
    manifest_path = output / "build-manifest.json"
    manifest: dict[str, object] = {
        "schemaVersion": "spec158-layered-build-v1",
        "buildId": output.name,
        "startedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "status": "RUNNING",
        "target": args.target,
        "jobs": args.jobs,
        "developmentCandidate": True,
        "liveGpuVerified": False,
        "published": False,
        "images": {},
        "executedProducts": [],
        "commands": [],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    started = time.monotonic()
    try:
        run_capture(["docker", "buildx", "inspect", "--bootstrap"])
        subprocess.run(
            [
                sys.executable,
                str(layered / "scripts/verify-layer-contract.py"),
                "--workspace", str(workspace),
                "--output", str(output / "layer-contract.json"),
            ],
            check=True,
        )
        platform_lock = locks / "platform.lock.json"
        ml_lock = locks / "ml-runtime.lock.json"
        ndn_lock = locks / "ndn-foundation.lock.json"
        app_lock = locks / "app-runtime.lock.json"
        lock_digests = {
            "platform": sha256(platform_lock),
            "ml": sha256(ml_lock),
            "ndn": sha256(ndn_lock),
            "app": sha256(app_lock),
        }
        manifest["lockDigests"] = lock_digests
        platform = json.loads(platform_lock.read_text())

        seal_script = layered / "scripts/prepare-layer-seals.py"
        seals = output / "seals"
        ndn_seal = seals / "ndn"
        app_seal = seals / "app"
        if args.target in {"ndn", "app", "all"}:
            subprocess.run([
                sys.executable, str(seal_script), "create-ndn",
                "--lock", str(ndn_lock), "--legacy-seal", str(workspace / ".spec110-build"),
                "--output", str(ndn_seal),
            ], check=True)
        if args.target in {"app", "all"}:
            subprocess.run([
                sys.executable, str(seal_script), "create-app",
                "--lock", str(app_lock), "--workspace", str(workspace),
                "--ndn-svs", str(Path(args.ndn_svs).resolve()),
                "--legacy-seal", str(workspace / ".spec110-build"),
                "--output", str(app_seal),
            ], check=True)
        ndn_seal_digest = (
            json.loads((ndn_seal / "seal.json").read_text())["sealDigest"]
            if ndn_seal.exists() else None
        )
        app_seal_value = (
            json.loads((app_seal / "seal.json").read_text())
            if app_seal.exists() else None
        )
        app_seal_digest = app_seal_value["sealDigest"] if app_seal_value else None
        manifest["sourceSeals"] = {
            "ndn": ndn_seal_digest,
            "app": app_seal_digest,
        }
        if app_seal_value:
            manifest["developmentCandidate"] = bool(
                app_seal_value.get("developmentCandidate")
            )

        ml_token = token(lock_digests["platform"], lock_digests["ml"])
        ml_tags = {
            "ml-devel": f"ndnsf-di-ml:spec158-{ml_token}-devel",
            "ml-runtime": f"ndnsf-di-ml:spec158-{ml_token}-runtime",
        }
        common_ml = {
            "PYTHON_BASE_IMAGE": platform["baseImages"]["python"],
            "GPU_BUILD_BASE_IMAGE": platform["baseImages"]["gpuBuild"],
            "GPU_RUNTIME_BASE_IMAGE": platform["baseImages"]["gpuRuntime"],
            "PLATFORM_LOCK_DIGEST": lock_digests["platform"],
            "ML_LOCK_DIGEST": lock_digests["ml"],
        }
        images: dict[str, object] = manifest["images"]  # type: ignore[assignment]
        if args.target in {"ml", "all"}:
            for product in ("ml-devel", "ml-runtime"):
                images[product] = build(
                    workspace=workspace,
                    dockerfile=layered / "Dockerfile.ml",
                    target=product,
                    tag=ml_tags[product],
                    args=common_ml,
                    contexts={},
                    log=output / f"{product}.log",
                    expected_labels={"org.ndnsf.di.layer": product},
                    dry_run=args.dry_run,
                )
                manifest["executedProducts"].append(product)  # type: ignore[union-attr]
        for product, tag in ml_tags.items():
            if product not in images:
                if args.dry_run and args.target == "all":
                    images[product] = {
                        "tag": tag,
                        "imageId": "dry-run:" + token(tag),
                        "action": "parent-placeholder",
                    }
                    continue
                if not existing(tag):
                    raise BuildError(f"ML_PARENT_MISSING:{tag}")
                images[product] = image_record(tag)
        ml_ids = {name: images[name]["imageId"] for name in ml_tags}  # type: ignore[index]

        if args.target in {"ndn", "app", "all"}:
            assert ndn_seal_digest is not None
            ndn_token = token(
                lock_digests["ndn"], ndn_seal_digest,
                str(ml_ids["ml-devel"]), str(ml_ids["ml-runtime"]),
            )
            ndn_tags = {
                "ndn-devel": f"ndnsf-di-ndn:spec158-{ndn_token}-devel",
                "ndn-runtime": f"ndnsf-di-ndn:spec158-{ndn_token}-runtime",
            }
            common_ndn = {
                "ML_DEVEL_IMAGE": ml_tags["ml-devel"],
                "ML_RUNTIME_IMAGE": ml_tags["ml-runtime"],
                "NDN_LOCK_DIGEST": lock_digests["ndn"],
                "NDN_SEAL_DIGEST": ndn_seal_digest,
                "BUILD_JOBS": str(args.jobs),
            }
            if args.target in {"ndn", "all"}:
                before = {
                    name: images[name]["imageId"] if args.dry_run
                    else image_record(tag)["imageId"]
                    for name, tag in ml_tags.items()
                }
                for product in ("ndn-devel", "ndn-runtime"):
                    images[product] = build(
                        workspace=workspace,
                        dockerfile=layered / "Dockerfile.ndn",
                        target=product,
                        tag=ndn_tags[product],
                        args=common_ndn,
                        contexts={"ndn_seal": ndn_seal},
                        log=output / f"{product}.log",
                        expected_labels={"org.ndnsf.di.layer": product},
                        dry_run=args.dry_run,
                    )
                    manifest["executedProducts"].append(product)  # type: ignore[union-attr]
                if not args.dry_run:
                    for name, tag in ml_tags.items():
                        assert_binding(tag, before[name])
            for product, tag in ndn_tags.items():
                if product not in images:
                    if args.dry_run and args.target == "all":
                        images[product] = {
                            "tag": tag,
                            "imageId": "dry-run:" + token(tag),
                            "action": "parent-placeholder",
                        }
                        continue
                    if not existing(tag):
                        raise BuildError(f"NDN_PARENT_MISSING:{tag}")
                    images[product] = image_record(tag)

        if args.target in {"app", "all"}:
            assert app_seal_digest is not None
            ndn_ids = {
                name: images[name]["imageId"] for name in ("ndn-devel", "ndn-runtime")  # type: ignore[index]
            }
            app_id = args.app_build_id or app_seal_digest.split(":", 1)[1][:12]
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", app_id):
                raise BuildError("APP_BUILD_ID_INVALID")
            app_tag = f"ndnsf-di:spec158-{app_id}"
            before = {
                **{
                    name: images[name]["imageId"] if args.dry_run
                    else image_record(tag)["imageId"]
                    for name, tag in ml_tags.items()
                },
                "ndn-devel": ndn_ids["ndn-devel"],
                "ndn-runtime": ndn_ids["ndn-runtime"],
            }
            images["app-runtime"] = build(
                workspace=workspace,
                dockerfile=layered / "Dockerfile.app",
                target="app-runtime",
                tag=app_tag,
                args={
                    "NDN_DEVEL_IMAGE": images["ndn-devel"]["tag"],  # type: ignore[index]
                    "NDN_RUNTIME_IMAGE": images["ndn-runtime"]["tag"],  # type: ignore[index]
                    "APP_LOCK_DIGEST": lock_digests["app"],
                    "APP_SEAL_DIGEST": app_seal_digest,
                    "APP_BUILD_ID": app_id,
                    "BUILD_JOBS": str(args.jobs),
                },
                contexts={"app_seal": app_seal},
                log=output / "app-runtime.log",
                expected_labels={
                    "org.ndnsf.di.layer": "app-runtime",
                    "org.ndnsf.di.app-build-id": app_id,
                },
                dry_run=args.dry_run,
            )
            manifest["executedProducts"].append("app-runtime")  # type: ignore[union-attr]
            if not args.dry_run:
                for name, expected in before.items():
                    tag = ml_tags[name] if name in ml_tags else images[name]["tag"]  # type: ignore[index]
                    assert_binding(tag, expected)
            if not args.dry_run:
                manifest["contentScan"] = scan_image(app_tag)
                manifest["staticProbe"] = static_probe(app_tag, output / "static-probe.json")

        manifest["status"] = "PASS"
    except Exception as error:
        manifest["status"] = "FAIL"
        manifest["reasonCode"] = str(error)
        manifest["finishedAt"] = dt.datetime.now(dt.timezone.utc).isoformat()
        manifest["durationSeconds"] = round(time.monotonic() - started, 3)
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        print(str(error), file=sys.stderr)
        return 4
    manifest["finishedAt"] = dt.datetime.now(dt.timezone.utc).isoformat()
    manifest["durationSeconds"] = round(time.monotonic() - started, 3)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
