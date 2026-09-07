"""Operator key material for the Spec180 protected-artifact registry.

The Spec180 trust-root registry (``contracts/trust-root-registry-v1.json``)
carries the authority identities and public-key digests; the private keys
live outside Git under ``~/.config/ndnsf/spec180/`` (mode 0600) exactly like
every other Spec180 signing key.

The requester (functional slice) loads the ``artifactPolicyAuthority``
private key in-process to issue grants over the existing publication path;
the Provider loads only the authority public key (checked against the
registry digest) plus its own recipient private key.
"""

from __future__ import annotations

import os
import hashlib
import json
import re
import stat
from dataclasses import dataclass
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, ec
from cryptography.hazmat.backends import default_backend

_DEFAULT_CONFIG_ROOT = Path.home() / ".config" / "ndnsf" / "spec180"
_ARTIFACT_POLICY_AUTHORITY_KEY = "artifact-policy-authority.key"


def _config_root(config_root: str | Path | None) -> Path:
    root = Path(config_root) if config_root is not None else Path(
        os.environ.get("NDNSF_SPEC180_CONFIG_ROOT", str(_DEFAULT_CONFIG_ROOT)))
    return root.expanduser()


def _check_private_mode(path: Path) -> None:
    """Fail closed unless the private key file is owner-only (mode 0600)."""
    mode = path.lstat().st_mode
    if not stat.S_ISREG(mode):
        raise ValueError(f"registry private key is not a regular file: {path}")
    if stat.S_IMODE(mode) != 0o600:
        raise ValueError(
            f"registry private key file is not mode 0600: {path}")


def load_artifact_policy_authority_private_key(
    config_root: str | Path | None = None,
    *, expected_public_key: ed25519.Ed25519PublicKey | None = None,
) -> ed25519.Ed25519PrivateKey:
    """Load the operator's artifact-policy authority private key (Ed25519)."""
    path = _config_root(config_root) / _ARTIFACT_POLICY_AUTHORITY_KEY
    if not path.is_file():
        raise FileNotFoundError(
            f"artifact policy authority private key is missing: {path}")
    _check_private_mode(path)
    key = serialization.load_pem_private_key(
        path.read_bytes(), password=None, backend=default_backend())
    if not isinstance(key, ed25519.Ed25519PrivateKey):
        raise TypeError("artifact policy authority key is not Ed25519")
    if expected_public_key is not None and _raw_public(key.public_key()) != _raw_public(expected_public_key):
        raise ValueError("authority private key does not match the registry public key")
    return key


def _raw_public(key: ed25519.Ed25519PublicKey) -> bytes:
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def load_ed25519_private_key(path: str | Path, *, raw_seed: bool = False):
    path = Path(path).expanduser()
    _check_private_mode(path)
    payload = path.read_bytes()
    key = (ed25519.Ed25519PrivateKey.from_private_bytes(payload) if raw_seed
           else serialization.load_pem_private_key(payload, password=None,
                                                    backend=default_backend()))
    if not isinstance(key, ed25519.Ed25519PrivateKey):
        raise ValueError("private key is not Ed25519")
    return key


def load_grant_recipient_private_key(path: str | Path):
    """Load a bounded owner-only Ed25519 or EC P-256 recipient PEM."""
    path = Path(path).expanduser()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o600
                or not 0 < info.st_size <= 65536):
            raise ValueError("recipient key must be a bounded regular file with mode 0600")
        payload = stream.read(65537)
        if len(payload) != info.st_size:
            raise ValueError("recipient key size changed during read")
    key = serialization.load_pem_private_key(
        payload, password=None, backend=default_backend())
    if isinstance(key, ed25519.Ed25519PrivateKey):
        return key
    if (isinstance(key, ec.EllipticCurvePrivateKey)
            and isinstance(key.curve, ec.SECP256R1)):
        return key
    raise ValueError("grant recipient private key must be Ed25519 or EC P-256")


def load_grant_recipient_public_map(path: str | Path):
    """Load pinned public recipients without opening any Provider private key.

    Map: absolute Provider name -> {path: relative PEM path, sha256: digest}.
    The operator must authenticate/bind the map itself in the candidate;
    matching its per-file hashes alone is not authorization.
    """
    path = Path(path).expanduser().absolute()

    def read_bounded(file, limit):
        if any(p.is_symlink() for p in (file, *file.parents)):
            raise ValueError('recipient public input is a symlink')
        fd = os.open(file, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        with os.fdopen(fd, 'rb') as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
                raise ValueError('recipient public input is not a bounded regular file')
            payload = stream.read(limit + 1)
            if len(payload) != info.st_size:
                raise ValueError('recipient public input size changed')
            return payload

    def unique(pairs):
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError('duplicate recipient map field')
            result[name] = value
        return result

    entries = json.loads(read_bounded(path, 1024 * 1024), object_pairs_hook=unique)
    if not isinstance(entries, dict) or not 1 <= len(entries) <= 64:
        raise ValueError('recipient public map cardinality')
    result = {}
    for provider, entry in entries.items():
        if (not isinstance(provider, str) or not provider.startswith('/') or provider == '/'
                or len(provider) > 1024 or any(ord(c) < 33 for c in provider)
                or not isinstance(entry, dict) or set(entry) != {'path', 'sha256'}
                or not isinstance(entry['path'], str) or not entry['path']
                or not isinstance(entry['sha256'], str)
                or not re.fullmatch(r'sha256:[0-9a-f]{64}', entry['sha256'])):
            raise ValueError('invalid recipient public map entry')
        relative = Path(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('recipient public path escapes map directory')
        payload = read_bounded(path.parent / relative, 65536)
        if 'sha256:' + hashlib.sha256(payload).hexdigest() != entry['sha256']:
            raise ValueError('recipient public key digest mismatch')
        key = serialization.load_pem_public_key(payload, backend=default_backend())
        if not (isinstance(key, ed25519.Ed25519PublicKey) or
                isinstance(key, ec.EllipticCurvePublicKey) and isinstance(key.curve, ec.SECP256R1)):
            raise ValueError('grant recipient public key must be Ed25519 or EC P-256')
        result[provider] = key
    return result


@dataclass(frozen=True)
class ArtifactPolicyRegistry:
    authority_id: str
    key_id: str
    public_key: ed25519.Ed25519PublicKey


def load_artifact_policy_authority_registry(
    registry_path: str | Path, *, model_family: str, protection_epoch: str,
) -> ArtifactPolicyRegistry:
    """Consume the pinned operator policy before protected grant use."""
    path = Path(registry_path).expanduser().resolve()
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 1 or document.get("status") != "CONFIGURED":
        raise ValueError("authority registry is not configured")
    policy = document.get("artifactPolicyAuthority", {})
    if (policy.get("publicKeyAlgorithm") != "ed25519"
            or policy.get("signatureAlgorithm") != "ed25519"
            or policy.get("grantSchema") != "ndnsf-di-key-grant-v1"):
        raise ValueError("authority registry algorithm or schema is unsupported")
    for field in ("authorityId", "keyId"):
        if not isinstance(policy.get(field), str) or not policy[field].strip():
            raise ValueError(f"authority registry lacks {field}")
    if model_family not in policy.get("acceptedModelFamilies", []):
        raise ValueError("model family is not authorized by the registry")
    if protection_epoch not in policy.get("protectionEpochs", []):
        raise ValueError("protection epoch is not authorized by the registry")
    relative = Path(policy.get("publicKeyPath", ""))
    root = path.parent.parent
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise ValueError("registry public key path is unsafe")
    public_path = (root / relative).resolve()
    try:
        public_path.relative_to(root)
    except ValueError as exc:
        raise ValueError("registry public key path escapes the feature root") from exc
    public_bytes = public_path.read_bytes()
    if "sha256:" + hashlib.sha256(public_bytes).hexdigest() != policy.get("publicKeySha256"):
        raise ValueError("authority public key digest differs from registry")
    key = serialization.load_pem_public_key(public_bytes, backend=default_backend())
    if not isinstance(key, ed25519.Ed25519PublicKey):
        raise ValueError("registry authority public key is not Ed25519")
    return ArtifactPolicyRegistry(policy["authorityId"], policy["keyId"], key)


def load_ed25519_public_key(path: str | Path) -> ed25519.Ed25519PublicKey:
    """Load a registry public key and verify it is an Ed25519 key."""
    key = serialization.load_pem_public_key(
        Path(path).read_bytes(), backend=default_backend())
    if not isinstance(key, ed25519.Ed25519PublicKey):
        raise TypeError(f"registry public key is not Ed25519: {path}")
    return key


__all__ = [
    "ArtifactPolicyRegistry", "load_artifact_policy_authority_registry",
    "load_ed25519_private_key", "load_grant_recipient_private_key",
    "load_grant_recipient_public_map",
    "load_artifact_policy_authority_private_key",
    "load_ed25519_public_key",
]
