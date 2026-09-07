"""Spec183 experiment-only signing authority (fixed, reusable key set).

Spec183 修整裁决 (2026-09-07): the Spec180 authority private keys are gone
from this machine (they were intentionally kept outside Git at
``~/.config/ndnsf/spec180/*.key`` per the catalogue-trust-root contract, and
are no longer present). Instead of waiting for an external owner re-issue,
Spec183 now owns one fixed experiment-only key set, kept under
``Experiments/TigerCluster/.keys/`` (never committed, mode 0600) and reused
for every run and every provider. Only the public identities, the trust-root
registry and their digests are committed, under the Spec183 contracts
directory.

The wire format is exactly the one ``scripts/spec180_contract_gate.py``
already verifies (shared by Spec180 T004/T005 and T012/T017):
  - the signature is a detached envelope on the manifest::
        {"signature": {"keyId": ..., "algorithm": "ed25519", "valueB64": ...}}
  - the signed payload is the manifest minus ``signature``, canonicalized as
    ``json.dumps(payload, ensure_ascii=False, sort_keys=True,
    separators=(",", ":"))``
  - verification binds keyId/algorithm and the public key to the checked-in
    trust-root registry; a manifest never carries its own authority.

This is an experiment-only trust root (the Spec180 revision-112 functional
profile): it makes no production PKI claim, and re-generating keys after the
registry is sealed would break every recorded signature, so ``issue`` is
fail-closed idempotent and never overwrites existing keys.

Layout
------
Private (git-ignored via ``Experiments/TigerCluster/.gitignore`` ``/.keys/``)::

    Experiments/TigerCluster/.keys/
        catalogue-authority.key            catalogue signer
        model-manifest-authority.key       model-manifest signer
        artifact-policy-authority.key      artifact-policy signer; the
                                           dispatch profile's protectionEpoch
                                           must be in its protectionEpochs
        offers/{role}.key                  fixed provider offer keys,
                                           one per role for every run

Public (committed under the Spec183 contracts directory)::

    specs/183-tiger-yolo-reusable-experiments/contracts/
        catalogue-authority.pub
        model-manifest-authority.pub
        artifact-policy-authority.pub
        offers/{role}.pub
        trust-root-registry-v1.json        schemaVersion 1, status CONFIGURED
        experiment-authority-v1.md         this arrangement as a contract
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys
from typing import Any, Iterable, Mapping

_REPO_ROOT = Path(__file__).resolve().parents[3]
KEY_DIR_REL = Path("Experiments/TigerCluster/.keys")
CONTRACTS_REL = Path("specs/183-tiger-yolo-reusable-experiments/contracts")
REGISTRY_FILENAME = "trust-root-registry-v1.json"

ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")
# (registry key, key-id, authority-id, manifest schema)
AUTHORITIES = (
    ("catalogue", "spec183-yolo-catalogue-ed25519-20260907",
     "spec183-yolo-catalogue-authority", "spec180-yolo-catalogue-v1"),
    ("modelManifest", "spec183-model-manifest-ed25519-20260907",
     "spec183-model-manifest-authority", "spec180-model-manifest-v1"),
)
# The offline issuer and the protected-grant path bind the artifact-policy
# authority through protectionEpochs; the Spec183 dispatch profile carries
# one of these epochs in security.protectionEpoch.
ARTIFACT_POLICY = {
    "registryKey": "artifactPolicyAuthority",
    "keyId": "spec183-artifact-policy-ed25519-20260907",
    "authorityId": "spec183-artifact-policy-authority",
    "grantSchema": "ndnsf-di-key-grant-v1",
    "protectionEpochs": ["spec183-yolo-protected-v1"],
}
ALGORITHM = "ed25519"
FAMILIES = ("YOLO26n",)
KEY_SLUGS = {"catalogue": "catalogue", "modelManifest": "model-manifest",
             "artifactPolicyAuthority": "artifact-policy"}


class AuthorityError(RuntimeError):
    pass


def _canonical_bytes(value: Any) -> bytes:
    """Deterministic JSON view signed by the Spec183/Spec180 authorities."""
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _sha256_digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _paths(root: Path) -> tuple[Path, Path, Path]:
    key_dir = root / KEY_DIR_REL
    contracts = root / CONTRACTS_REL
    registry = contracts / REGISTRY_FILENAME
    return key_dir, contracts, registry


def _crypto():
    """Lazy imports keep the module importable where cryptography is absent."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
    return serialization, ed25519


def _load_pem_key(path: Path):
    serialization, _ = _crypto()
    data = path.read_bytes()
    try:
        from cryptography.hazmat.backends import default_backend
        return serialization.load_pem_private_key(
            data, password=None, backend=default_backend())
    except TypeError:  # modern cryptography removed the backend argument
        return serialization.load_pem_private_key(data, password=None)


def _load_pem_public(pem: bytes):
    serialization, _ = _crypto()
    try:
        from cryptography.hazmat.backends import default_backend
        return serialization.load_pem_public_key(pem, backend=default_backend())
    except TypeError:  # modern cryptography removed the backend argument
        return serialization.load_pem_public_key(pem)


def _pem_public(key) -> bytes:
    serialization, _ = _crypto()
    return key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)


def sign_manifest(manifest: Mapping[str, Any], *, key_path: Path,
                  key_id: str, authority_id: str | None = None) -> dict[str, Any]:
    """Return a copy of ``manifest`` with the detached signature envelope.

    ``authority_id`` is optional: the Spec183 wire format carries keyId only,
    but the DI catalogue verifier additionally binds ``authorityId`` from the
    registry, so catalogue re-signing passes it through.
    """
    payload = dict(manifest)
    payload.pop("signature", None)
    key = _load_pem_key(key_path)
    signature = key.sign(_canonical_bytes(payload))
    signed = dict(manifest)
    signed["signature"] = {
        "keyId": key_id,
        "algorithm": ALGORITHM,
        "valueB64": base64.b64encode(signature).decode("ascii"),
    }
    if authority_id is not None:
        signed["signature"]["authorityId"] = authority_id
    return signed


def _issue_keypair(key_dir: Path, name: str, *, force: bool = False) -> bytes:
    """Create one 0600 ed25519 private key under ``key_dir`` (idempotent).

    Never overwrites an existing key unless ``force`` is set; returns the PEM
    public identity either way.
    """
    _, ed25519 = _crypto()
    serialization, _ = _crypto()
    key_dir.mkdir(parents=True, exist_ok=True)
    key_dir.chmod(0o700)
    private = key_dir / (name + ".key")
    if private.exists():
        if not force:
            return _pem_public(_load_pem_key(private))
        private.unlink()
    key = ed25519.Ed25519PrivateKey.generate()
    private.write_bytes(key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    ))
    private.chmod(0o600)
    return _pem_public(key)


def _write_committed(path: Path, payload: bytes, *, force: bool = False) -> bool:
    """Write a public/registry artifact; refuse to change existing content."""
    if path.exists():
        if path.read_bytes() == payload:
            return False
        if not force:
            raise AuthorityError(
                f"refusing to overwrite existing committed artifact {path}; "
                "pass --force only after recording the identity change")
        path.write_bytes(payload)
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return True


def issue(root: Path, *, force: bool = False) -> dict[str, Any]:
    """Create or confirm the whole fixed key set; report identities.

    Fail-closed: existing private keys are never regenerated; an existing
    registry whose key IDs differ from this tool's raises unless ``force``.
    """
    key_dir, contracts, registry = _paths(root)
    key_dir.mkdir(parents=True, exist_ok=True)
    key_dir.chmod(0o700)
    identities: dict[str, Any] = {"keyDir": str(KEY_DIR_REL),
                                  "authorities": {}, "offers": {}}

    for name, key_id, _authority_id, _schema in AUTHORITIES:
        slug = KEY_SLUGS[name]
        pub_pem = _issue_keypair(key_dir, slug + "-authority", force=force)
        pub_path = contracts / (slug + "-authority.pub")
        _write_committed(pub_path, pub_pem, force=force)
        identities["authorities"][name] = {
            "keyId": key_id,
            "publicKeyPath": "contracts/" + pub_path.name,
            "publicKeySha256": _sha256_digest(pub_pem),
        }
    policy = ARTIFACT_POLICY
    policy_slug = KEY_SLUGS[policy["registryKey"]]
    pub_pem = _issue_keypair(key_dir, policy_slug + "-authority", force=force)
    pub_path = contracts / (policy_slug + "-authority.pub")
    _write_committed(pub_path, pub_pem, force=force)
    identities["authorities"][policy["registryKey"]] = {
        "keyId": policy["keyId"],
        "publicKeyPath": "contracts/" + pub_path.name,
        "publicKeySha256": _sha256_digest(pub_pem),
    }

    offer_dir = key_dir / "offers"
    offer_pub_dir = contracts / "offers"
    serialization, _ = _crypto()
    for role in ROLES:
        pub_pem = _issue_keypair(offer_dir, role, force=force)
        _write_committed(offer_pub_dir / (role + ".pub"), pub_pem, force=force)
        parsed = _load_pem_public(pub_pem)
        raw = parsed.public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        identities["offers"][role] = {
            "keyId": _sha256_digest(raw),
            "publicKeyPath": "contracts/offers/" + role + ".pub",
            "publicKeySha256": _sha256_digest(pub_pem),
        }

    registry_doc: dict[str, Any] = {"schemaVersion": 1, "status": "CONFIGURED"}
    for name, key_id, authority_id, schema in AUTHORITIES:
        info = identities["authorities"][name]
        registry_doc[name] = {
            "authorityId": authority_id,
            "keyId": key_id,
            "publicKeyAlgorithm": ALGORITHM,
            "signatureAlgorithm": ALGORITHM,
            "publicKeyPath": info["publicKeyPath"],
            "publicKeySha256": info["publicKeySha256"],
            "manifestSchema": schema,
            "acceptedModelFamilies": list(FAMILIES),
        }
    policy_info = identities["authorities"][policy["registryKey"]]
    registry_doc[policy["registryKey"]] = {
        "authorityId": policy["authorityId"],
        "keyId": policy["keyId"],
        "publicKeyAlgorithm": ALGORITHM,
        "signatureAlgorithm": ALGORITHM,
        "publicKeyPath": policy_info["publicKeyPath"],
        "publicKeySha256": policy_info["publicKeySha256"],
        "grantSchema": policy["grantSchema"],
        "acceptedModelFamilies": list(FAMILIES),
        "protectionEpochs": list(policy["protectionEpochs"]),
    }
    all_names = [name for name, *_ in AUTHORITIES] + [policy["registryKey"]]
    if registry.exists():
        existing = json.loads(registry.read_text())
        for name in all_names:
            current = existing.get(name)
            if (current is not None
                    and current.get("keyId") != registry_doc[name]["keyId"]):
                if not force:
                    raise AuthorityError(
                        f"registry {registry} already names a different {name} "
                        "key; refusing to mix identities (pass --force after "
                        "recording the change)")
        if existing != registry_doc:
            # Additive registration of a brand-new fixed authority needs no
            # force; changing an already-registered keyId does (checked above).
            registry.write_text(json.dumps(registry_doc, indent=1) + "\n")
    else:
        registry.parent.mkdir(parents=True, exist_ok=True)
        registry.write_text(json.dumps(registry_doc, indent=1) + "\n")
    return identities


def _trust_entry(registry: Mapping[str, Any], key: str) -> Mapping[str, Any]:
    entry = registry.get(key)
    if not isinstance(entry, Mapping):
        raise AuthorityError(f"registry has no configured {key} entry")
    return entry


def verify_signed_manifest(manifest: Mapping[str, Any], *, root: Path,
                           authority: str) -> tuple[bool, list[str]]:
    """Verify against the committed registry; mirrors the shared gate."""
    from cryptography.exceptions import InvalidSignature
    _, _, registry_path = _paths(root)
    registry = json.loads(registry_path.read_text())
    entry = _trust_entry(registry, authority)
    problems: list[str] = []
    signature = manifest.get("signature")
    if not isinstance(signature, Mapping):
        return False, ["MANIFEST_UNSIGNED"]
    if signature.get("keyId") != entry.get("keyId"):
        problems.append("MANIFEST_UNKNOWN_KEY")
    if signature.get("algorithm") != entry.get("signatureAlgorithm"):
        problems.append("MANIFEST_SIGNATURE_ALGORITHM")
    if manifest.get("modelFamily") not in entry.get("acceptedModelFamilies", ()):
        problems.append("MANIFEST_MODEL_FAMILY")
    value_b64 = signature.get("valueB64")
    if not isinstance(value_b64, str) or not value_b64:
        problems.append("MANIFEST_SIGNATURE_MISSING")
    feature_dir = root / CONTRACTS_REL.parent  # registry sits in feature contracts
    pub_path = feature_dir / str(entry.get("publicKeyPath", ""))
    if not pub_path.is_file():
        problems.append("TRUST_ROOT_PUBLIC_KEY_MISSING")
    if problems:
        return False, problems
    if _sha256_digest(pub_path.read_bytes()) != entry.get("publicKeySha256"):
        return False, ["TRUST_ROOT_PUBLIC_KEY_DIGEST"]
    public_key = _load_pem_public(pub_path.read_bytes())
    payload = dict(manifest)
    payload.pop("signature", None)
    try:
        public_key.verify(base64.b64decode(value_b64, validate=True),
                          _canonical_bytes(payload))
    except (InvalidSignature, ValueError, Exception) as exc:  # backend broadness
        return False, ["MANIFEST_SIGNATURE_INVALID", str(exc)]
    return True, []


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    issue_p = sub.add_parser("issue", help="create/confirm the fixed key set")
    issue_p.add_argument("--root", default=None, help="repo root (default: derived)")
    issue_p.add_argument("--force", action="store_true",
                         help="regenerate keys / overwrite public identities")
    issue_p.add_argument("--json", action="store_true", help="print identities as JSON")

    sign_p = sub.add_parser("sign", help="sign one manifest with a fixed key")
    sign_p.add_argument("--root", default=None, help="repo root (default: derived)")
    sign_p.add_argument("--key", required=True, help="private key path")
    sign_p.add_argument("--key-id", required=True, help="registered keyId")
    sign_p.add_argument("--manifest", required=True, help="input manifest JSON")
    sign_p.add_argument("--output", required=True, help="output signed manifest JSON")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve() if args.root else _REPO_ROOT
    if args.command == "issue":
        identities = issue(root, force=args.force)
        if args.json:
            print(json.dumps(identities, indent=1))
        else:
            for name, info in identities["authorities"].items():
                print(f"{name:14s} keyId={info['keyId']}  "
                      f"sha256={info['publicKeySha256']}")
            for role, info in identities["offers"].items():
                print(f"offer {role:14s} keyId={info['keyId']}")
            print("private keys:", identities["keyDir"], "(mode 0600, git-ignored)")
        return 0
    if args.command == "sign":
        manifest = json.loads(Path(args.manifest).read_text())
        signed = sign_manifest(manifest, key_path=Path(args.key).resolve(),
                               key_id=args.key_id)
        Path(args.output).write_text(json.dumps(signed, indent=1) + "\n")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
