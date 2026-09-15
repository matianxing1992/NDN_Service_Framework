#!/usr/bin/env python3
"""Prepare portable, commit-pinned inputs for the existing local SIF builder.

This tool never builds an image or submits a job. Source validation is separate
from the host qualification manifest required by build-local-sif.sh.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
TEMPLATE = HERE.parent / "templates" / "development-runtime.def.in"
SCHEMA = "ndnsf-development-handoff-v1"
LOCK_SCHEMA = "ndnsf-development-lock-v1"
REPOSITORIES = ("ndnsf", "nacAbe", "ndnSvs", "ndnSd")
REQUIRED_WHEELS = frozenset({
    "pybind11-2.13.6-py3-none-any.whl",
    "python_ndn-0.3-py3-none-any.whl",
    "pygtrie-2.5.0-py3-none-any.whl",
    "aenum-3.1.17-py3-none-any.whl",
    "pycryptodomex-3.23.0-cp37-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
})


def fail(reason):
    raise ValueError(reason)


def digest(path):
    """Hash one input without loading an image into memory."""
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return "sha256:" + value.hexdigest()


def write_json(path, data):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(data, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def load_lock(path):
    """Require full Git and asset identities, with no executable config fields."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if data.get("schema") != LOCK_SCHEMA:
        fail("HANDOFF_LOCK_SCHEMA")
    if set(data.get("repositories", {})) != set(REPOSITORIES):
        fail("HANDOFF_REPOSITORIES")
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", data.get("release", "")):
        fail("HANDOFF_RELEASE")
    for name, row in data["repositories"].items():
        if not re.fullmatch(r"[0-9a-f]{40}", row.get("revision", "")):
            fail("HANDOFF_REVISION:" + name)
    wheels = data.get("wheels")
    if not isinstance(wheels, list) or not wheels:
        fail("HANDOFF_WHEELS")
    seen = set()
    for row in wheels:
        name = row.get("filename", "")
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.whl", name) or name in seen:
            fail("HANDOFF_WHEEL_NAME")
        seen.add(name)
    if seen != REQUIRED_WHEELS:
        fail("HANDOFF_REQUIRED_WHEELS")
    for row in [data.get("baseSif", {}), *wheels]:
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", row.get("sha256", "")):
            fail("HANDOFF_ASSET_DIGEST")
    return data


def git(workspace, *args):
    return subprocess.check_output(["git", "-C", str(workspace), *args],
                                   text=True, stderr=subprocess.PIPE).strip()


def reject_symlink_components(path, code):
    """Reject compatibility links before resolving a build input path."""
    path = Path(path)
    if not path.is_absolute():
        fail(code + "_NOT_ABSOLUTE")
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            fail(code + "_SYMLINK:" + str(current))


def check_checkout(workspace, revision):
    """Pin the actual checkout; untracked logs do not authorize untracked code."""
    if git(workspace, "rev-parse", "HEAD") != revision:
        fail("HANDOFF_CHECKOUT_REVISION:" + str(workspace))
    if git(workspace, "status", "--porcelain", "--untracked-files=no"):
        fail("HANDOFF_CHECKOUT_DIRTY:" + str(workspace))


def check_selected(workspace, selected):
    tracked = set(git(workspace, "ls-files", "-z").split("\0"))
    for relative in selected:
        resolved = (workspace / relative).resolve()
        try:
            canonical = resolved.relative_to(workspace).as_posix()
        except ValueError:
            fail("HANDOFF_SOURCE_OUTSIDE_CHECKOUT:" + str(relative))
        if canonical not in tracked:
            fail("HANDOFF_SOURCE_UNTRACKED:" + canonical)


def prepare(lock_path, workspaces, wheels, output):
    """Create one source-only bundle using the maintained dependency sealer."""
    lock_path = Path(lock_path).resolve()
    lock = load_lock(lock_path)
    workspaces = {key: Path(value).resolve() for key, value in workspaces.items()}
    output = Path(output).resolve()
    if output.exists():
        fail("HANDOFF_OUTPUT_EXISTS")
    sealer = module("handoff_source_sealer", HERE / "prepare-local-sif-source.py")
    selectors = {
        "ndnsf": lambda p: sealer.selected_files(p),
        "nacAbe": lambda p: sealer.selected_dependency_files(p, sealer.NAC_ABE_FILES),
        "ndnSvs": lambda p: sealer.selected_dependency_files(
            p, tuple(entry for entry in sealer.NDN_SVS_FILES if entry != "VERSION.info")),
        "ndnSd": lambda p: sealer.selected_dependency_files(p, sealer.NDNSD_FILES),
    }
    for name in REPOSITORIES:
        check_checkout(workspaces[name], lock["repositories"][name]["revision"])
        check_selected(workspaces[name], selectors[name](workspaces[name]))
    for row in lock["wheels"]:
        source = Path(wheels) / row["filename"]
        if not source.is_file() or digest(source) != row["sha256"]:
            fail("HANDOFF_WHEEL_DIGEST:" + row["filename"])
    output.mkdir(parents=True)
    subprocess.run([
        sys.executable, str(HERE / "prepare-local-sif-source.py"),
        "--workspace", str(workspaces["ndnsf"]),
        "--nac-abe-workspace", str(workspaces["nacAbe"]),
        "--ndn-svs-workspace", str(workspaces["ndnSvs"]),
        "--derive-ndn-svs-version",
        "--ndnsd-workspace", str(workspaces["ndnSd"]),
        "--output-dir", str(output / "source"),
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    for name in REPOSITORIES:
        check_checkout(workspaces[name], lock["repositories"][name]["revision"])
    seal_path = output / "source/source-seal.json"
    seal = json.loads(seal_path.read_text(encoding="utf-8"))
    seal["workspace"] = "git:" + seal["sourceRevision"]
    seal["archive"]["path"] = "workspace.tar"
    for name, row in seal["dependencies"].items():
        row["workspace"] = "git:" + row["sourceRevision"]
        row["archive"]["path"] = "ndn-svs.tar" if name == "ndnSvs" else name + ".tar"
    # Only operational paths change; the existing path-independent seal stays.
    seal_path.write_text(json.dumps(seal, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output / "wheels").mkdir()
    for row in lock["wheels"]:
        shutil.copyfile(Path(wheels) / row["filename"], output / "wheels" / row["filename"])
    shutil.copyfile(TEMPLATE, output / "runtime.def.in")
    shutil.copyfile(lock_path, output / "dependency-lock.json")
    files = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob("*")) if p.is_file()}
    write_json(output / "handoff.json", {
        "schema": SCHEMA, "status": "SOURCE_READY", "release": lock["release"],
        "repositories": lock["repositories"], "files": files,
        "sourceSealDigest": seal["sealDigest"], "baseSif": lock["baseSif"],
        "sifBuild": "NOT_RUN", "runtimeQualification": "NOT_RUN",
    })
    return verify(output)


def verify(bundle):
    """Check package bytes and source archives after relocation, without writes."""
    bundle = Path(bundle).resolve()
    report = json.loads((bundle / "handoff.json").read_text(encoding="utf-8"))
    if report.get("schema") != SCHEMA or report.get("status") != "SOURCE_READY":
        fail("HANDOFF_MANIFEST_SCHEMA")
    files = report.get("files", {})
    required = {"dependency-lock.json", "runtime.def.in", "source/source-seal.json",
                "source/workspace.tar", "source/nacAbe.tar", "source/ndn-svs.tar", "source/ndnSd.tar"}
    if not isinstance(files, dict) or not required.issubset(files):
        fail("HANDOFF_MANIFEST_FILES")
    observed = set()
    for path in bundle.rglob("*"):
        if path.is_symlink():
            fail("HANDOFF_UNEXPECTED_SYMLINK:" + str(path.relative_to(bundle)))
        if path.is_file():
            observed.add(str(path.relative_to(bundle)))
    if observed != set(files) | {"handoff.json"}:
        fail("HANDOFF_UNEXPECTED_FILE")
    for name, expected in files.items():
        path = bundle / name
        if Path(name).is_absolute() or ".." in Path(name).parts or path.is_symlink():
            fail("HANDOFF_FILE_PATH:" + name)
        try:
            path.resolve().relative_to(bundle)
        except ValueError:
            fail("HANDOFF_FILE_PATH:" + name)
        if not path.is_file() or digest(path) != expected:
            fail("HANDOFF_FILE_DIGEST:" + name)
    lock = load_lock(bundle / "dependency-lock.json")
    if (report.get("repositories") != lock["repositories"] or
            report.get("baseSif") != lock["baseSif"] or report.get("release") != lock["release"]):
        fail("HANDOFF_LOCK_MISMATCH")
    for row in lock["wheels"]:
        if files.get("wheels/" + row["filename"]) != row["sha256"]:
            fail("HANDOFF_WHEEL_MANIFEST")
    validator = module("handoff_source_validator", HERE / "validate-local-sif-source.py")
    checked = validator.validate(bundle / "source/source-seal.json")
    seal = json.loads((bundle / "source/source-seal.json").read_text(encoding="utf-8"))
    actual = {"ndnsf": seal["sourceRevision"],
              **{key: value["sourceRevision"] for key, value in seal["dependencies"].items()}}
    expected = {key: value["revision"] for key, value in lock["repositories"].items()}
    if actual != expected or checked["sealDigest"] != report.get("sourceSealDigest"):
        fail("HANDOFF_SOURCE_REVISION_MISMATCH")
    return {"status": "SOURCE_READY", "release": lock["release"],
            "files": len(files), "sourceSealDigest": checked["sealDigest"],
            "sifBuild": "NOT_RUN", "runtimeQualification": "NOT_RUN"}


def render(bundle, base_sif, destination):
    """Resolve machine-specific paths after verifying the exact dependency base."""
    raw_bundle, raw_base_sif = Path(bundle), Path(base_sif)
    reject_symlink_components(raw_bundle, "HANDOFF_BUNDLE_PATH")
    reject_symlink_components(raw_base_sif, "HANDOFF_BASE_SIF_PATH")
    bundle, base_sif = raw_bundle.resolve(), raw_base_sif.resolve()
    checked = verify(bundle)
    lock = load_lock(bundle / "dependency-lock.json")
    if not base_sif.is_file():
        fail("HANDOFF_BASE_SIF_MISSING")
    if digest(base_sif) != lock["baseSif"]["sha256"]:
        fail("HANDOFF_BASE_DIGEST")
    for path in [bundle, base_sif]:
        if not re.fullmatch(r"/[A-Za-z0-9_./+-]+", str(path)):
            fail("HANDOFF_DEFINITION_PATH_REQUIRES_SIMPLE_ABSOLUTE_PATH")
    text = (bundle / "runtime.def.in").read_text(encoding="utf-8")
    for token, value in {"@BUNDLE@": str(bundle), "@BASE_SIF@": str(base_sif),
                         "@SEAL_DIGEST@": checked["sourceSealDigest"],
                         "@RELEASE@": lock["release"]}.items():
        text = text.replace(token, value)
    if re.search(r"@[A-Z_]+@", text):
        fail("HANDOFF_TEMPLATE_TOKEN")
    destination = Path(destination).resolve()
    if bundle == destination or bundle in destination.parents:
        fail("HANDOFF_RENDER_OUTPUT_MUST_BE_OUTSIDE_BUNDLE")
    with destination.open("x", encoding="utf-8") as stream:
        stream.write(text)
    boundary = module("handoff_boundary", ROOT / "packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py")
    boundary.validate_definition(destination)
    return {**checked, "definition": str(destination), "definitionSha256": digest(destination),
            "buildEntry": "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh",
            "hostGateManifest": "REQUIRED_FOR_BUILD"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    prep = modes.add_parser("prepare")
    prep.add_argument("--lock", required=True, type=Path)
    for name in ["ndnsf", "nac-abe", "ndn-svs", "ndnsd"]:
        prep.add_argument("--" + name + "-workspace", required=True, type=Path)
    prep.add_argument("--wheels", required=True, type=Path)
    prep.add_argument("--output", required=True, type=Path)
    check = modes.add_parser("verify")
    check.add_argument("--bundle", required=True, type=Path)
    rendering = modes.add_parser("render")
    rendering.add_argument("--bundle", required=True, type=Path)
    rendering.add_argument("--base-sif", required=True, type=Path)
    rendering.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "prepare":
            result = prepare(args.lock, {"ndnsf": args.ndnsf_workspace,
                "nacAbe": args.nac_abe_workspace, "ndnSvs": args.ndn_svs_workspace,
                "ndnSd": args.ndnsd_workspace},
                args.wheels, args.output)
        elif args.mode == "render":
            result = render(args.bundle, args.base_sif, args.output)
        else:
            result = verify(args.bundle)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("HANDOFF_FAILED:" + str(error), file=sys.stderr)
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
