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
RUNTIME_LOCK_SCHEMA = "ndnsf-development-lock-v2"
RUNTIME_PROFILE = "di-runtime"
LEGACY_PROFILE = "di-development-with-explicit-fixtures"
REPOSITORIES = ("ndnsf", "nacAbe", "ndnSvs", "ndnSd")
REQUIRED_WHEELS = frozenset({
    "pybind11-2.13.6-py3-none-any.whl",
    "python_ndn-0.3-py3-none-any.whl",
    "pygtrie-2.5.0-py3-none-any.whl",
    "aenum-3.1.17-py3-none-any.whl",
    "pycryptodomex-3.23.0-cp37-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
})
NATIVE_FILES = frozenset({'onnx.tar', 'vendor.tar', 'native-inputs.json',
    'cargo-1.90.0-x86_64-unknown-linux-gnu.tar.xz',
    'rustc-1.90.0-x86_64-unknown-linux-gnu.tar.xz',
    'rust-std-1.90.0-x86_64-unknown-linux-gnu.tar.xz'})
TOKENIZER_PREFIX = 'NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/'
TOKENIZER_FILES = ('Cargo.toml', 'Cargo.lock', 'src/lib.rs', 'tokenizer-abi.h')


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
    runtime = data.get("schema") == RUNTIME_LOCK_SCHEMA
    if data.get("schema") not in (LOCK_SCHEMA, RUNTIME_LOCK_SCHEMA):
        fail("HANDOFF_LOCK_SCHEMA")
    if set(data.get("repositories", {})) != ({"ndnsf"} if runtime else set(REPOSITORIES)):
        fail("HANDOFF_REPOSITORIES")
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]*", data.get("release", "")):
        fail("HANDOFF_RELEASE")
    for name, row in data["repositories"].items():
        if not re.fullmatch(r"[0-9a-f]{40}", row.get("revision", "")):
            fail("HANDOFF_REVISION:" + name)
    wheels = data.get("wheels", []) if runtime else data.get("wheels")
    if not isinstance(wheels, list) or (not wheels and not runtime):
        fail("HANDOFF_WHEELS")
    seen = set()
    for row in wheels:
        name = row.get("filename", "")
        if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.whl", name) or name in seen:
            fail("HANDOFF_WHEEL_NAME")
        seen.add(name)
    if seen != (set() if runtime else REQUIRED_WHEELS):
        fail("HANDOFF_REQUIRED_WHEELS")
    assets = [data.get("baseSif", {}), *wheels]
    if runtime:
        assets.append(data.get("baseSdk", {}))
    for row in assets:
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", row.get("sha256", "")):
            fail("HANDOFF_ASSET_DIGEST")
    native = data.get('nativeBuild', {}).get('files', {})
    expected_native = {"native-inputs.json"} if runtime else NATIVE_FILES
    if set(native) != expected_native or any(not re.fullmatch(r'sha256:[0-9a-f]{64}', v)
                                        for v in native.values()):
        fail('HANDOFF_NATIVE_INPUTS')
    return data


def check_runtime_sdk(lock, path):
    """Bind the selected metadata to the exact base and tokenizer declaration.

    Image execution still runs the base's verifier against its actual SDK files.
    This source-only check does not qualify the base ABI or inference behavior.
    """
    if digest(path) != lock["baseSdk"]["sha256"]:
        fail("HANDOFF_BASE_SDK_DIGEST")
    sdk = json.loads(Path(path).read_text(encoding="utf-8"))
    expected = lock["nativeBuild"]["files"]["native-inputs.json"]
    if (sdk.get("schema") != "ndnsf-base-sdk-v1" or
            "sha256:" + sdk.get("inputs", {}).get("files", {}).get(
                "native/native-inputs.json", "") != expected):
        fail("HANDOFF_BASE_SDK_NATIVE_BINDING")


def require_tokenizer_sdk(path, source_hashes):
    """Check the declared bridge closure; actual native checks remain in the base."""
    sdk = json.loads(Path(path).read_text())
    bridge = sdk.get('tokenizerBridge', {})
    archive = '/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a'
    expected = bridge.get('archiveSha256', '')
    if (bridge.get('status') != 'VERIFIED' or bridge.get('nativeProbes') != 'PASS' or
            not re.fullmatch('[0-9a-f]{64}', expected) or
            sdk.get('artifacts', {}).get(archive) != expected):
        fail('HANDOFF_TOKENIZER_SDK_MISSING')
    for name in TOKENIZER_FILES:
        if source_hashes.get(TOKENIZER_PREFIX + name) != (
                'sha256:' + bridge.get('inputs', {}).get('crate/' + name, '')):
            fail('HANDOFF_TOKENIZER_SOURCE_MISMATCH:' + name)


def require_nac_sdk(path, source_hashes):
    """Bind current host-locked patches and the required native API to the base SDK."""
    sdk = json.loads(Path(path).read_text())
    nac = sdk.get('nacAbePatchClosure', {})
    library = '/opt/ndn-base/lib/libnac-abe.so'
    expected = nac.get('installedFiles', {}).get(library, '')
    suites = ['TestProducerBoundCk', 'TestConsumerRevocation', 'TestConsumer', 'TestProducer']
    if (nac.get('abi') != 'VERIFIED' or nac.get('nativeTests') != 'PASS' or
            nac.get('nativeSuites') != suites or not re.fullmatch('[0-9a-f]{64}', expected) or
            sdk.get('artifacts', {}).get(library) != expected):
        fail('HANDOFF_NAC_SDK_MISSING')
    needed = {'packaging/host-dependencies.lock.json', 'scripts/stack_sources.py',
              'patches/nac-abe/series.json', 'patches/openabe/series.json'}
    needed.update(name for name in source_hashes
                  if name.startswith(('patches/nac-abe/', 'patches/openabe/')) and
                  name.endswith('.patch'))
    if set(nac.get('sourceFiles', {})) != needed or len(needed) <= 4:
        fail('HANDOFF_NAC_SOURCE_SET')
    for name in needed:
        if source_hashes.get(name) != nac['sourceFiles'][name]:
            fail('HANDOFF_NAC_SOURCE_MISMATCH:' + name)
    if (source_hashes['patches/nac-abe/series.json'] != 'sha256:' + nac.get('patchSeriesSha256', '') or
            source_hashes['packaging/host-dependencies.lock.json'] !=
            'sha256:' + nac.get('hostDependencyLockSha256', '') or
            nac.get('upstreamCommit') != sdk.get('inputs', {}).get('repositories', {}).get(
                'nacAbe', {}).get('revision')):
        fail('HANDOFF_NAC_IDENTITY')


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


def prepare(lock_path, workspaces, wheels, output, native_inputs=None):
    """Create one source-only bundle using the maintained dependency sealer."""
    lock_path = Path(lock_path).resolve()
    lock = load_lock(lock_path)
    if lock["schema"] != LOCK_SCHEMA:
        fail("HANDOFF_RUNTIME_REQUIRES_PREPARE_RUNTIME")
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
    native_inputs = Path(native_inputs) if native_inputs else Path(wheels).parent / 'native-inputs'
    for name, expected in lock['nativeBuild']['files'].items():
        if not (native_inputs / name).is_file() or digest(native_inputs / name) != expected:
            fail('HANDOFF_NATIVE_DIGEST:' + name)
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
    (output / 'native').mkdir()
    for name in NATIVE_FILES:
        shutil.copyfile(native_inputs / name, output / 'native' / name)
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


def prepare_runtime(workspace, base_sif, base_sdk, native_inputs, output):
    """Seal only repository source; external SDK payload stays in the base SIF."""
    workspace, base_sif, base_sdk, native_inputs, output = map(
        Path, (workspace, base_sif, base_sdk, native_inputs, output))
    for path in (workspace, base_sif, base_sdk, native_inputs, output):
        reject_symlink_components(path, "HANDOFF_RUNTIME_INPUT")
    if output.exists():
        fail("HANDOFF_OUTPUT_EXISTS")
    if not all(path.is_file() for path in (base_sif, base_sdk, native_inputs)):
        fail("HANDOFF_RUNTIME_INPUT_MISSING")
    revision = git(workspace, "rev-parse", "HEAD")
    lock = {
        "schema": RUNTIME_LOCK_SCHEMA, "release": "ndnsf-di-runtime",
        "repositories": {"ndnsf": {"revision": revision}},
        "baseSif": {"sha256": digest(base_sif)},
        "baseSdk": {"sha256": digest(base_sdk)},
        "nativeBuild": {"files": {"native-inputs.json": digest(native_inputs)}},
    }
    check_runtime_sdk(lock, base_sdk)
    require_tokenizer_sdk(base_sdk, {
        TOKENIZER_PREFIX + name: digest(workspace / (TOKENIZER_PREFIX + name))
        for name in TOKENIZER_FILES})
    selected = module('runtime_source_selection', HERE / 'prepare-local-sif-source.py')
    require_nac_sdk(base_sdk, {str(name): digest(workspace / name)
                              for name in selected.selected_files(workspace)})
    output.mkdir(parents=True)
    subprocess.run([
        sys.executable, str(HERE / "prepare-local-sif-source.py"),
        "--workspace", str(workspace), "--output-dir", str(output / "source"),
    ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    seal_path = output / "source/source-seal.json"
    seal = json.loads(seal_path.read_text(encoding="utf-8"))
    if seal["sourceRevision"] != revision or seal.get("dependencies"):
        fail("HANDOFF_RUNTIME_SOURCE_CHANGED")
    seal["workspace"] = "git:" + revision
    seal["archive"]["path"] = "workspace.tar"
    seal_path.write_text(json.dumps(seal, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output / "native").mkdir()
    shutil.copyfile(native_inputs, output / "native/native-inputs.json")
    shutil.copyfile(base_sdk, output / "base-sdk.json")
    shutil.copyfile(TEMPLATE, output / "runtime.def.in")
    write_json(output / "dependency-lock.json", lock)
    files = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob("*"))
             if p.is_file()}
    write_json(output / "handoff.json", {
        "schema": SCHEMA, "status": "SOURCE_READY", "release": lock["release"],
        "runtimeProfile": RUNTIME_PROFILE, "repositories": lock["repositories"],
        "files": files, "sourceSealDigest": seal["sealDigest"], "baseSif": lock["baseSif"],
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
    profile = report.get("runtimeProfile", LEGACY_PROFILE)
    if profile not in (RUNTIME_PROFILE, LEGACY_PROFILE):
        fail("HANDOFF_RUNTIME_PROFILE")
    required = {"dependency-lock.json", "runtime.def.in", "source/source-seal.json",
                "source/workspace.tar"}
    required |= ({"base-sdk.json", "native/native-inputs.json"} if profile == RUNTIME_PROFILE else
                 {"source/nacAbe.tar", "source/ndn-svs.tar", "source/ndnSd.tar"})
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
    if (lock["schema"] == RUNTIME_LOCK_SCHEMA) != (profile == RUNTIME_PROFILE):
        fail("HANDOFF_RUNTIME_PROFILE_LOCK")
    if (report.get("repositories") != lock["repositories"] or
            report.get("baseSif") != lock["baseSif"] or report.get("release") != lock["release"]):
        fail("HANDOFF_LOCK_MISMATCH")
    for row in lock.get("wheels", []):
        if files.get("wheels/" + row["filename"]) != row["sha256"]:
            fail("HANDOFF_WHEEL_MANIFEST")
    for name, expected in lock['nativeBuild']['files'].items():
        if files.get('native/' + name) != expected:
            fail('HANDOFF_NATIVE_MANIFEST:' + name)
    if profile == RUNTIME_PROFILE:
        check_runtime_sdk(lock, bundle / "base-sdk.json")
    validator = module("handoff_source_validator", HERE / "validate-local-sif-source.py")
    checked = validator.validate(bundle / "source/source-seal.json")
    seal = json.loads((bundle / "source/source-seal.json").read_text(encoding="utf-8"))
    actual = {"ndnsf": seal["sourceRevision"],
              **{key: value["sourceRevision"] for key, value in seal.get("dependencies", {}).items()}}
    expected = {key: value["revision"] for key, value in lock["repositories"].items()}
    if actual != expected or checked["sealDigest"] != report.get("sourceSealDigest"):
        fail("HANDOFF_SOURCE_REVISION_MISMATCH")
    return {"status": "SOURCE_READY", "release": lock["release"], "runtimeProfile": profile,
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
    if checked['runtimeProfile'] == RUNTIME_PROFILE:
        seal = json.loads((bundle / 'source/source-seal.json').read_text())
        require_tokenizer_sdk(bundle / 'base-sdk.json',
                              {row['path']: row['sha256'] for row in seal['files']})
        require_nac_sdk(bundle / 'base-sdk.json',
                        {row['path']: row['sha256'] for row in seal['files']})
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
                         "@BASE_DIGEST@": lock["baseSif"]["sha256"],
                         "@NATIVE_DIGEST@": lock['nativeBuild']['files']['native-inputs.json'],
                         "@RUNTIME_PROFILE@": checked["runtimeProfile"],
                         "@LOCK_DIGEST@": digest(bundle / "dependency-lock.json"),
                         "@BASE_SDK_DIGEST@": lock.get("baseSdk", {}).get("sha256", "legacy-archives"),
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
            "hostGateManifest": "REQUIRED_FOR_RELEASE_PROMOTION", "buildOnlySupported": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    prep = modes.add_parser("prepare")
    prep.add_argument("--lock", required=True, type=Path)
    for name in ["ndnsf", "nac-abe", "ndn-svs", "ndnsd"]:
        prep.add_argument("--" + name + "-workspace", required=True, type=Path)
    prep.add_argument("--wheels", required=True, type=Path)
    prep.add_argument('--native-inputs', required=True, type=Path)
    prep.add_argument("--output", required=True, type=Path)
    runtime = modes.add_parser("prepare-runtime", help="Seal NDNSF against an existing base SDK")
    for name in ("workspace", "base-sif", "base-sdk", "native-inputs", "output"):
        runtime.add_argument("--" + name, required=True, type=Path)
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
                args.wheels, args.output, args.native_inputs)
        elif args.mode == "prepare-runtime":
            result = prepare_runtime(args.workspace, args.base_sif, args.base_sdk,
                                     args.native_inputs, args.output)
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
