#!/usr/bin/env python3
"""Prepare one fail-closed Spec180 YOLO case input bundle.

The canonical package and catalogue signature already exist before this tool
runs.  This tool creates only case policy, topology, Provider-offer keys and a
digest manifest.  Private keys must live outside the repository and are never
copied into the public bundle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
from typing import Any

from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
)


ROOT = Path(__file__).resolve().parents[2]
SERVICE = "/AI/YOLO/YOLO26n"
CONTROLLER = "/example/controller"
USER = "/example/user"
GROUP = "/example/group"
PROVIDER_PREFIX = "/example/provider"
CASES = {
    "Y-A": {
        "candidate": "atomic-v1",
        "providers": (("FullModel", ("FullModel",)),),
    },
    "Y-B": {
        "candidate": "shared-backbone-two-shard-v1",
        "providers": (
            ("BackboneNeck", ("BackboneNeck",)),
            ("DetectShard0", ("DetectShard0",)),
            ("DetectShard1", ("DetectShard1",)),
            ("Merge", ("Merge",)),
        ),
    },
    # Four identities: a distinct shared-role cover plus at least one
    # FullModel capability, per the T011 Y-N cardinality/cover contract.
    "Y-N": {
        "candidate": "shared-backbone-two-shard-v1",
        "providers": (
            ("BackboneNeck", ("FullModel", "BackboneNeck")),
            ("DetectShard0", ("DetectShard0",)),
            ("DetectShard1", ("DetectShard1",)),
            ("Merge", ("Merge",)),
        ),
    },
}


class PreparationError(RuntimeError):
    pass


def _repo_service_names() -> tuple[str, ...]:
    """Use the repository's canonical service registry, never a copied list."""
    repo_python = ROOT / "NDNSF-DistributedRepo/pythonWrapper"
    if str(repo_python) not in sys.path:
        sys.path.insert(0, str(repo_python))
    from py_repoclient.service_names import repo_versioned_services

    return tuple(repo_versioned_services())


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _write_json(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )


def _is_beneath(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _load_verified_package(package: Path, registry: Path):
    if not package.is_dir() or not registry.is_file():
        raise PreparationError("canonical package or catalogue registry is missing")
    sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter

    try:
        adapter = build_yolo26n_adapter(package, registry_path=registry)
    except Exception as exc:
        raise PreparationError("signed canonical package verification failed") from exc
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    return adapter, manifest


def _private_key(path: Path) -> Ed25519PrivateKey:
    if path.exists():
        try:
            key = serialization.load_pem_private_key(
                path.read_bytes(), password=None, backend=default_backend())
        except (OSError, TypeError, ValueError) as exc:
            raise PreparationError("existing Provider offer key is invalid") from exc
        if not isinstance(key, Ed25519PrivateKey):
            raise PreparationError("existing Provider offer key is not Ed25519")
        return key
    key = Ed25519PrivateKey.generate()
    path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    path.chmod(0o600)
    return key


def prepare_case(
    case: str,
    package: Path,
    registry: Path,
    output: Path,
    private_key_root: Path,
    topology_source: Path,
) -> dict[str, Any]:
    if case not in CASES:
        raise PreparationError("only registered cases may be prepared at this gate")
    package = package.expanduser().resolve()
    registry = registry.expanduser().resolve()
    output = output.expanduser().resolve()
    private_key_root = private_key_root.expanduser().resolve()
    topology_source = topology_source.expanduser().resolve()
    if _is_beneath(private_key_root, ROOT):
        raise PreparationError("private Provider offer keys must remain outside the repository")
    if output.exists() and any(output.iterdir()):
        raise PreparationError("case input output directory is not empty")
    if not topology_source.is_file():
        raise PreparationError("topology source is missing")

    _, package_manifest = _load_verified_package(package, registry)
    catalogue = package_manifest["catalogue"]
    by_id = {item["candidateId"]: item for item in catalogue["candidates"]}
    case_spec = CASES[case]
    candidate_id = str(case_spec["candidate"])
    if candidate_id not in by_id:
        raise PreparationError("registered case candidate is absent from package")
    candidate_digest = str(by_id[candidate_id]["candidateDigest"])

    output.mkdir(parents=True, exist_ok=True)
    public_dir = output / "offer-public-keys"
    public_dir.mkdir()
    private_key_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    private_key_root.chmod(0o700)

    providers = []
    private_map: dict[str, str] = {}
    public_map: dict[str, str] = {}
    trust_entries = []
    provider_nodes: dict[str, str] = {}
    # One NFD/one node-scoped PIB per MiniNDN node: co-locating several
    # Providers on one node makes them race on the shared SQLite PIB
    # ("database is locked").  Spread Providers across distinct topology
    # nodes; memphis stays reserved for Controller/User, so the cycling
    # starts from ucla and continues through neu/arizona/wustl.
    provider_node_pool = ["ucla", "neu", "arizona", "wustl"]
    for index, (suffix, roles) in enumerate(case_spec["providers"]):
        identity = PROVIDER_PREFIX + "/" + str(suffix)
        key_path = private_key_root / (str(suffix) + ".pem")
        key = _private_key(key_path)
        if key_path.stat().st_mode & 0o077:
            raise PreparationError("Provider offer private-key permissions are too broad")
        public = key.public_key()
        public_raw = public.public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        public_pem = public.public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo)
        key_id = "sha256:" + hashlib.sha256(public_raw).hexdigest()
        public_path = public_dir / (str(suffix) + ".pub")
        public_path.write_bytes(public_pem)
        public_map[key_id] = str(public_path)
        private_map[identity] = str(key_path)
        providers.append({"identity": identity, "roles": list(roles)})
        provider_nodes[identity] = provider_node_pool[
            index % len(provider_node_pool)]
        locator = identity + "/KEY/spec180"
        trust_entries.append({
            "provider": identity,
            "service": SERVICE,
            "keyLocatorPrefix": identity + "/KEY/",
            "signerKeyId": key_id,
            "certificateName": locator + "/self/v1",
        })

    repo_identity = PROVIDER_PREFIX + "/Repo"
    repo_clients = sorted({
        CONTROLLER, USER, repo_identity,
        *(item["identity"] for item in providers),
    })
    inference_service = {
        "name": SERVICE,
        "model": "/Model/YOLO26n",
        "roles": [role for _, roles in case_spec["providers"] for role in roles],
        "users": [USER],
        "providers": providers,
    }
    repo_services = [{
        "name": name,
        "model": name,
        "roles": [],
        "users": repo_clients,
        "providers": [{"identity": repo_identity, "roles": []}],
        "dependencies": [],
    } for name in _repo_service_names()]
    config = {
        "application": "spec180-yolo",
        "controller": CONTROLLER,
        "group": GROUP,
        "services": [inference_service, *repo_services],
        "runtime": {
            "user_identity": USER,
            "provider_prefix": PROVIDER_PREFIX,
            "nodes": {
                "controller": "memphis",
                "user": "memphis",
                "repo": "neu",
                "providers": provider_nodes,
            },
            "identities": {
                "controller": CONTROLLER,
                "user": USER,
                "repo": repo_identity,
                "group": GROUP,
                "providerPrefix": PROVIDER_PREFIX,
                "repoServicePrefix": "/NDNSF/DistributedRepo/Object",
                "providers": {item["identity"]: item["identity"] for item in providers},
            },
        },
    }
    trust_root = {
        "schema": "spec180-provider-offer-trust-v1",
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "trustSchema": CONTROLLER + "/KEY",
        "entries": trust_entries,
    }

    config_path = output / "case-config.json"
    topology_path = output / "topology.conf"
    trust_path = output / "offer-trust-root.json"
    public_map_path = output / "offer-public-key-map.json"
    private_map_path = output / "offer-private-key-map.json"
    _write_json(config_path, config)
    shutil.copyfile(topology_source, topology_path)
    _write_json(trust_path, trust_root)
    _write_json(public_map_path, public_map)
    _write_json(private_map_path, private_map)
    private_map_path.chmod(0o600)

    record = {
        "schema": "spec180-yolo-case-bundle-v1",
        "case": case,
        "candidateId": candidate_id,
        "candidateDigest": candidate_digest,
        "package": str(package),
        "files": {
            "packageManifest": _sha256(package / "manifest.json"),
            "catalogueRegistry": _sha256(registry),
            "config": _sha256(config_path),
            "topology": _sha256(topology_path),
            "offerTrustRoot": _sha256(trust_path),
            "offerPublicKeyMap": _sha256(public_map_path),
            "offerPrivateKeyMap": _sha256(private_map_path),
            "offerPublicKeys": {
                key_id: _sha256(Path(path)) for key_id, path in sorted(public_map.items())
            },
            "offerPrivateKeys": {
                identity: _sha256(Path(path)) for identity, path in sorted(private_map.items())
            },
        },
    }
    _write_json(output / "case-bundle.json", record)
    return record


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", required=True, choices=sorted(CASES))
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--registry", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--private-key-root", required=True, type=Path)
    parser.add_argument(
        "--topology", type=Path,
        default=ROOT / "Experiments/Topology/AI_Lab.conf")
    args = parser.parse_args(argv)
    record = prepare_case(
        args.case, args.package, args.registry, args.out_dir,
        args.private_key_root, args.topology)
    print("SPEC180_YOLO_CASE_INPUTS " + json.dumps({
        "case": record["case"],
        "candidateId": record["candidateId"],
        "candidateDigest": record["candidateDigest"],
        "bundle": str(args.out_dir.expanduser().resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
