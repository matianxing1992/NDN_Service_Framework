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
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.backends import default_backend

_DEFAULT_CONFIG_ROOT = Path.home() / ".config" / "ndnsf" / "spec180"
_ARTIFACT_POLICY_AUTHORITY_KEY = "artifact-policy-authority.key"


def _config_root(config_root: str | Path | None) -> Path:
    root = Path(config_root) if config_root is not None else Path(
        os.environ.get("NDNSF_SPEC180_CONFIG_ROOT", str(_DEFAULT_CONFIG_ROOT)))
    return root.expanduser()


def _check_private_mode(path: Path) -> None:
    """Fail closed unless the private key file is owner-only (mode 0600)."""
    if path.stat().st_mode & 0o077:
        raise ValueError(
            f"registry private key file is not mode 0600: {path}")


def load_artifact_policy_authority_private_key(
    config_root: str | Path | None = None,
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
    return key


def load_ed25519_public_key(path: str | Path) -> ed25519.Ed25519PublicKey:
    """Load a registry public key and verify it is an Ed25519 key."""
    key = serialization.load_pem_public_key(
        Path(path).read_bytes(), backend=default_backend())
    if not isinstance(key, ed25519.Ed25519PublicKey):
        raise TypeError(f"registry public key is not Ed25519: {path}")
    return key


__all__ = [
    "load_artifact_policy_authority_private_key",
    "load_ed25519_public_key",
]
