"""Spec183 runtime + dispatch content planes: render/check from real artifacts.

Builds the two later-stage content planes and the sealed harness under
``Experiments/TigerCluster/.cache/planes/``, the same CAS convention as
``spec183_inputs_plane.py`` (hard links, never symlinks, fail-closed on a
changed source).  The dispatch plane is the published release surface, so
``render`` deterministically rebuilds it: repeated renders on unchanged
sources produce identical bytes and therefore identical plane identities.

Stage contents (``runtime/yolo_profile.REQUIRED_FILES``):

    runtime   sif            .cache/base-sif/spec180-runtime.sif (b6710fd6)
              nativeManifest .cache/native-manifest/container-native-build.json
                             (extracted from /opt/ndnsf-di/current/manifest/
                             inside the base SIF; extract-native subcommand)
              libraryLock    .cache/handoff/development-20260906/
                             dependency-lock.json (build-time dependency lock)
    dispatch  effectiveProfile  generated effective-behavior document
              harnessManifest   explicit sealed harness under ``harness/``
              modelManifest     signed shared-backbone model manifest
              oracle            canonical oracle full-model-output.npy
              fixture           fixed fixture PPM (committed under tests/)
              trustPolicy       committed trust-root registry
              validationContract committed experiment-profile contract

The effectiveProfile document is the contract's "effective behavior" of the
profile: every file row is reduced to ``{bytes, sha256}`` (paths never
participate in identities) and ``release`` (the plane references, which would
otherwise form a profile -> plane -> profile self-reference), the physical
``authorityPrivateKey``/``apptainer`` locators, and the storage roots are
excluded -- exactly the reduction ``resolve_run_plan`` applies to profile
behavior.  Because release rows are excluded from the document, synchronizing
real row hashes back into the profile never changes the document, so the
render -> row-sync loop converges in one pass.

``render`` also writes the exact ``release.inputs/runtime/dispatch`` and
``evidence.harnessManifest`` row values back into the profile file, so the
profile is always self-consistent with the rendered planes.  ``check`` runs
the production validators (``check_chain`` through dispatch plus
``verify_harness``); VERIFIED here is content integrity only and never a
runtime qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable

_TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TOOL_DIR.parent))  # TigerCluster: runtime.*, tools.*
_REPO_ROOT = Path(__file__).resolve().parents[3]
PLANE_ROOT_REL = Path("Experiments/TigerCluster/.cache/planes")
NATIVE_REL = Path("Experiments/TigerCluster/.cache/native-manifest/container-native-build.json")
BASE_SIF_REL = Path("Experiments/TigerCluster/.cache/base-sif/spec180-runtime.sif")
PROFILE_REL = Path("Experiments/TigerCluster/profiles/yolo-two-node.json")
DEFAULT_APPTAINER = "/opt/apptainer/1.5.3/bin/apptainer"
BASE_SIF_SHA256 = "sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285"

# (plane row name, real source file relative to the repo root)
RUNTIME_SOURCES = {
    "sif": BASE_SIF_REL,
    "nativeManifest": NATIVE_REL,
    "libraryLock": Path("Experiments/TigerCluster/.cache/handoff/development-20260906/"
                        "dependency-lock.json"),
}
DISPATCH_SOURCES = {
    "modelManifest": Path("Experiments/TigerCluster/.cache/model/spec180-public/"
                          "model-manifest-shared-backbone-two-shard-v1.signed.json"),
    "oracle": Path("Experiments/TigerCluster/.cache/model/spec180-public/models/"
                   "canonical-package/oracle/full-model-output.npy"),
    "fixture": Path("tests/fixtures/spec180/yolo26n/fixed-fixture.ppm"),
    "trustPolicy": Path("specs/183-tiger-yolo-reusable-experiments/contracts/"
                        "trust-root-registry-v1.json"),
    "validationContract": Path("specs/183-tiger-yolo-reusable-experiments/contracts/"
                               "experiment-profile.md"),
}
# Plane rows whose real files are generated/derived, not linked from one CAS file.
HARNESS_REL = Path("harness")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(4 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _link_into(source: Path, target: Path, name: str) -> dict:
    """Hard-link one CAS source under a plane root; never copy or symlink."""
    if not source.is_file() or source.is_symlink():
        raise RuntimeError(f"real source missing for {name}: {source}")
    target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if any(p.is_symlink() for p in (target, *target.parents)):
        raise RuntimeError(f"plane target lies below a symlink: {target}")
    try:
        os.link(source, target)
    except FileExistsError:
        pass  # identical previous render already linked it
    if not (target.stat().st_nlink > 1 or target == source):
        raise RuntimeError(f"plane file {name} is not the CAS file")
    observed = _sha256(target)
    if observed != _sha256(source):
        raise RuntimeError(f"plane file {name} does not match its source ({observed})")
    return {"path": target.name, "bytes": target.stat().st_size,
            "sha256": observed}


def _logical(item):
    """Reduce file rows to identity and recurse (mirrors resolve_run_plan)."""
    if isinstance(item, dict):
        if set(item) == {"path", "bytes", "sha256"}:
            return {"bytes": item["bytes"], "sha256": item["sha256"]}
        return {key: _logical(entry) for key, entry in item.items()}
    return item


def _effective_document(profile: dict) -> dict:
    """Deterministic effective-behavior document without self-references.

    Excludes ``release`` (plane references would form a profile -> plane ->
    profile self-reference), the physical ``apptainer``/``authorityPrivateKey``
    locators, and all storage roots except the byte budgets -- the same
    reduction ``resolve_run_plan`` applies to profile behavior.
    """
    behavior = _logical(profile)
    behavior.pop("release", None)
    behavior["runtime"].pop("apptainer", None)
    behavior["security"].pop("authorityPrivateKey", None)
    behavior["storage"] = {key: behavior["storage"][key]
                           for key in ("peakBytes", "marginBytes")}
    return {"schema": "tiger-yolo-effective-profile-v1",
            "profileId": profile.get("profileId"),
            "effectiveBehavior": behavior}


def _plane_document(stage: str, parent_id, files: dict, parameters: dict) -> dict:
    return {"schema": "tiger-yolo-plane-v1", "stage": stage, "parentId": parent_id,
            "files": files, "parameters": parameters}


def _plane_row(root: Path, name: str) -> dict:
    path = root / name
    return {"path": name, "bytes": path.stat().st_size, "sha256": _sha256(path)}


def _stage_id(root: Path, stage: str, *, parent_id) -> str:
    from runtime.yolo_profile import check_plane
    return check_plane(root / "plane.json", expected_stage=stage,
                       parent_id=parent_id)["id"]


def _sealed_harness(dispatch_root: Path) -> dict:
    """Freeze the declared harness under ``dispatch_root/harness`` (read-only)."""
    from runtime.yolo_bundle import REQUIRED_HARNESS_FILES, MANIFEST
    from runtime.yolo_bundle import freeze_harness, harness_source
    base = _REPO_ROOT / "Experiments/TigerCluster"
    rows = {}
    for name in sorted(REQUIRED_HARNESS_FILES):
        source = harness_source(base, name)
        if not source.is_file() or source.is_symlink():
            raise RuntimeError(f"harness source missing: {source}")
        rows[name] = {"bytes": source.stat().st_size, "sha256": _sha256(source)}
    document = {"schema": "tiger-yolo-harness-v1", "files": rows}
    raw = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
    manifest_path = dispatch_root / MANIFEST  # transient; replaced by the seal
    manifest_path.write_bytes(raw)
    digest = _sha256(manifest_path)
    frozen = freeze_harness(manifest_path, dispatch_root / HARNESS_REL,
                            expected_manifest_sha256=digest, source_root=base)
    manifest_path.unlink()  # the read-only copy under harness/ is authoritative
    return frozen


def _rmtree_force(root: Path) -> None:
    """Remove a tree that may contain a sealed read-only harness.

    A freeze makes every harness directory read-only; unlinking a file is
    governed by its parent directory's write bit, so grant u+w on each
    directory before ``rmtree`` (files themselves need no change).
    """
    if not root.exists():
        return
    import stat as stat_module
    for directory in (root, *[p for p in root.rglob("*") if p.is_dir()]):
        try:
            directory.chmod(directory.stat().st_mode | stat_module.S_IWUSR)
        except OSError:
            pass  # already gone or unwritable for another reason; rmtree reports
    shutil.rmtree(root)


def _sync_profile_rows(profile_path: Path, plane_root: Path) -> list[str]:
    """Rewrite every file row to the real identity of the file it points at.

    The profile is the single editable publication; all row hashes must equal
    the referenced file on disk.  release.{inputs,runtime,dispatch} point at
    the rendered plane manifests, evidence.harnessManifest at the sealed copy,
    and every other row at its real committed/CAS source.  A missing target
    fails closed instead of leaving a stale declaration.
    """
    profile = json.loads(profile_path.read_text())
    base = profile_path.resolve().parent
    updated: list[str] = []

    def walk(item, where: str):
        if isinstance(item, dict):
            if set(item) == {"path", "bytes", "sha256"}:
                path = Path(item["path"])
                target = path if path.is_absolute() else base / path
                if any(p.is_symlink() for p in (target, *target.parents)):
                    raise RuntimeError(f"profile row {where} lies below a symlink")
                if not target.is_file():
                    raise RuntimeError(f"profile row {where} target missing: {target}")
                observed = _sha256(target)
                if item.get("sha256") != observed or item.get("bytes") != target.stat().st_size:
                    updated.append(where)
                    item["bytes"] = target.stat().st_size
                    item["sha256"] = observed
                return
            for key, entry in item.items():
                walk(entry, where + "." + key)
        elif isinstance(item, list):
            for index, entry in enumerate(item):
                walk(entry, f"{where}[{index}]")

    walk(profile, "profile")
    payload = json.dumps(profile, indent=1) + "\n"
    if profile_path.read_text() != payload:
        profile_path.write_text(payload)
    return updated


def render(plane_root: Path, profile_path: Path) -> dict:
    """Render runtime + dispatch planes and synchronize the profile rows."""
    plane_root = Path(plane_root).resolve()
    if any(p.is_symlink() for p in (plane_root, *plane_root.parents)):
        raise RuntimeError(f"plane root lies below a symlink: {plane_root}")
    inputs_root, runtime_root = plane_root / "inputs", plane_root / "runtime"
    dispatch_root = plane_root / "dispatch"
    from runtime.yolo_profile import check_chain
    check_chain({"inputs": inputs_root / "plane.json"}, through="inputs")
    inputs_id = _stage_id(inputs_root, "inputs", parent_id=None)

    # Runtime plane: the three real CAS sources, hard-linked.
    if runtime_root.exists():
        _rmtree_force(runtime_root)
    runtime_root.mkdir(mode=0o700)
    runtime_files = {}
    for name, relative in RUNTIME_SOURCES.items():
        runtime_files[name] = _link_into(
            _REPO_ROOT / relative, runtime_root / relative.name, name)
    _write_plane(runtime_root, "runtime", inputs_id, runtime_files)
    runtime_id = _stage_id(runtime_root, "runtime", parent_id=inputs_id)

    # Dispatch plane: effective-behavior document, sealed harness, real sources.
    if dispatch_root.exists():
        _rmtree_force(dispatch_root)
    dispatch_root.mkdir(mode=0o700)
    profile = json.loads(profile_path.read_text())
    effective = dispatch_root / "effective-profile.json"
    effective.write_text(json.dumps(_effective_document(profile), indent=1) + "\n")
    _sealed_harness(dispatch_root)
    dispatch_files = {}
    for name, relative in DISPATCH_SOURCES.items():
        dispatch_files[name] = _link_into(
            _REPO_ROOT / relative, dispatch_root / relative.name, name)
    dispatch_files["effectiveProfile"] = {
        "path": effective.name, "bytes": effective.stat().st_size,
        "sha256": _sha256(effective)}
    sealed_manifest = dispatch_root / HARNESS_REL / "harness-manifest.json"
    dispatch_files["harnessManifest"] = {
        "path": str(HARNESS_REL / "harness-manifest.json"),
        "bytes": sealed_manifest.stat().st_size, "sha256": _sha256(sealed_manifest)}
    _write_plane(dispatch_root, "dispatch", runtime_id, dispatch_files)
    dispatch_id = _stage_id(dispatch_root, "dispatch", parent_id=runtime_id)

    updated = _sync_profile_rows(profile_path, plane_root)
    return {"status": "RENDERED", "planeRoot": str(plane_root),
            "runtime": runtime_id, "dispatch": dispatch_id,
            "inputs": inputs_id, "profileRowsUpdated": updated}


def _write_plane(root: Path, stage: str, parent_id, files: dict) -> None:
    """Write plane.json; its stage id is the canonical document sha, which
    ``check_plane`` recomputes (never the raw file bytes)."""
    payload = json.dumps(_plane_document(stage, parent_id, files, {}),
                         sort_keys=True, separators=(",", ":")).encode()
    (root / "plane.json").write_bytes(payload)


def check(plane_root: Path) -> dict:
    """Run the production validators across the whole dispatch chain."""
    from runtime.yolo_bundle import verify_harness
    from runtime.yolo_profile import check_chain, ClosureError
    try:
        plane_root = Path(plane_root).resolve()
        dispatch_root = plane_root / "dispatch"
        chain = check_chain({"inputs": plane_root / "inputs/plane.json",
                             "runtime": plane_root / "runtime/plane.json",
                             "dispatch": dispatch_root / "plane.json"},
                            through="dispatch")
        manifest = dispatch_root / HARNESS_REL / "harness-manifest.json"
        harness = verify_harness(manifest.parent,
                                 expected_manifest_sha256=_sha256(manifest))
        return {"status": "VERIFIED", "stage": "dispatch",
                "identities": chain["identities"], "harness": harness,
                "qualification": "NOT_EVALUATED"}
    except (ClosureError, RuntimeError, OSError) as exc:
        return {"status": "INVALID", "integrity": "REJECTED", "reason": str(exc)}


def extract_native(*, sif: Path, target: Path, apptainer: str) -> dict:
    """Extract the native build manifest from inside the base SIF (one time)."""
    if sif.is_symlink() or not sif.is_file():
        raise RuntimeError(f"base SIF unavailable: {sif}")
    if _sha256(sif) != BASE_SIF_SHA256:
        raise RuntimeError("base SIF digest differs from the recorded identity")
    if target.exists():
        return {"status": "PRESENT", "path": str(target)}
    with tempfile.TemporaryDirectory(prefix="ndnsf-extract-") as home:
        command = [apptainer, "exec", "--home", home, str(sif), "sh", "-c",
                   "cat /opt/ndnsf-di/current/manifest/container-native-build.json"]
        try:
            payload = subprocess.run(command, capture_output=True, check=True,
                                     timeout=300).stdout
        except (OSError, subprocess.CalledProcessError,
                subprocess.TimeoutExpired) as exc:
            raise RuntimeError(f"cannot extract native manifest: {exc}") from exc
    if not payload:
        raise RuntimeError("extracted native manifest is empty")
    target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    target.write_bytes(payload)
    target.chmod(0o444)
    return {"status": "EXTRACTED", "path": str(target),
            "sha256": _sha256(target), "bytes": target.stat().st_size}


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (("render", "rebuild planes and sync profile rows"),
                            ("check", "validate the whole dispatch chain")):
        child = sub.add_parser(name, help=help_text)
        child.add_argument("--root", default=None,
                           help="planes root (default: repo .cache/planes)")
        if name == "render":
            child.add_argument("--profile", default=None,
                               help="profile path (default: repo profiles/yolo-two-node.json)")
    native = sub.add_parser("extract-native",
                            help="extract container-native-build.json from the base SIF")
    native.add_argument("--apptainer", default=DEFAULT_APPTAINER)
    native.add_argument("--sif", default=None)
    args = parser.parse_args(argv)
    if args.command == "extract-native":
        sif = Path(args.sif).resolve() if args.sif else _REPO_ROOT / BASE_SIF_REL
        result = extract_native(sif=sif, target=_REPO_ROOT / NATIVE_REL,
                                apptainer=args.apptainer)
        print(json.dumps(result, sort_keys=True))
        return 0
    root = Path(args.root or str(_REPO_ROOT / PLANE_ROOT_REL))
    if not root.is_absolute():
        root = _REPO_ROOT / root
    if args.command == "check":
        report = check(root)
        print(json.dumps(report, sort_keys=True))
        return 0 if report.get("status") == "VERIFIED" else 2
    profile = Path(args.profile or str(_REPO_ROOT / PROFILE_REL))
    if not profile.is_absolute():
        profile = (_REPO_ROOT / profile).resolve() if args.profile else profile
    if args.profile and not profile.exists():
        profile = (_REPO_ROOT / Path(args.profile)).resolve()
    if not profile.is_file():
        raise SystemExit(f"profile not found: {profile}")
    result = render(root, profile)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
