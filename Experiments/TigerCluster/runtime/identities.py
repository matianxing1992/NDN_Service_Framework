"""Offline per-run issuer using the exact SIF's ndnsec implementation.

Each role keeps only its own private key. The issuer handles certificate
requests and returns public certificates, without exporting its root key.
"""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import stat
import subprocess

from runtime.baseline import ROLE_RANK, write_json, digest


def validate_role_homes(homes: dict[str, Path]) -> dict[str, Path]:
    """Check prepared HOME/PIB isolation without opening SQLite or private keys.

    This is a filesystem check, not certificate validation or a concurrency
    lease. The worker must keep these run-private paths stable after checking.
    """
    if not isinstance(homes, dict) or not homes:
        raise ValueError("ROLE_HOMES")
    checked, pib_inodes, private_inodes = {}, set(), set()
    for role, value in homes.items():
        if not isinstance(role, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", role):
            raise ValueError("ROLE_NAME")
        if not isinstance(value, (str, Path)):
            raise ValueError("ROLE_HOME_PATH:" + role)
        home = Path(value)
        if not home.is_absolute() or ".." in home.parts or home.name != role:
            raise ValueError("ROLE_HOME_PATH:" + role)
        pib = home / ".ndn/pib.db"
        if any(path.is_symlink() for path in (pib, *pib.parents)):
            raise ValueError("ROLE_HOME_SYMLINK:" + role)
        if not home.is_dir() or not pib.is_file():
            raise ValueError("ROLE_PIB_MISSING:" + role)
        info = pib.stat()
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("ROLE_PIB_TYPE:" + role)
        inode = (info.st_dev, info.st_ino)
        if inode in pib_inodes:
            raise ValueError("SHARED_PIB:" + role)
        if any(home == other or home in other.parents or other in home.parents
               for other in checked.values()):
            raise ValueError("SHARED_HOME:" + role)
        tpm = home / ".ndn/ndnsec-key-file"
        if tpm.is_symlink() or not tpm.is_dir():
            raise ValueError("ROLE_TPM_PATH:" + role)
        keys = list(tpm.glob("*.privkey"))
        if not keys:
            raise ValueError("ROLE_TPM_MISSING:" + role)
        for key in keys:
            info = key.lstat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError("ROLE_PRIVATE_KEY_TYPE:" + role)
            key_inode = (info.st_dev, info.st_ino)
            if key_inode in private_inodes:
                raise ValueError("SHARED_PRIVATE_KEY:" + role)
            private_inodes.add(key_inode)
        checked[role] = home
        pib_inodes.add(inode)
    return checked


def install_public(home: Path, certificates: list[Path], own_identity: str) -> None:
    """Populate public-only PIB entries before any runtime opens the database.

    ndnsec cert-install in this image requires the destination identity/key to
    exist. python-ndn uses the same PIB schema; insert public key bits and use
    its certificate API. Never call touch_identity/new_key for a peer identity.
    """
    from ndn.encoding import Name, parse_data
    from ndn.security.keychain.keychain_sqlite3 import KeychainSqlite3
    from ndn.security.tpm.tpm_file import TpmFile
    pib = KeychainSqlite3(str(home / ".ndn/pib.db"), TpmFile(str(home / ".ndn/ndnsec-key-file")))
    before = sorted(p.name for p in (home / ".ndn/ndnsec-key-file").glob("*.privkey"))
    try:
        for path in certificates:
            wire = base64.b64decode(path.read_bytes())
            name, _, content, _ = parse_data(wire)
            identity, key = name[:-4], name[:-2]
            if Name.to_str(identity) == own_identity:
                continue
            public_identity = pib.new_identity(identity)
            key_wire = Name.to_bytes(key)
            pib.conn.execute("INSERT INTO keys (identity_id,key_name,key_bits) VALUES (?,?,?)",
                             (public_identity.row_id, key_wire, bytes(content)))
            pib.conn.commit()
            pib.import_cert(key_wire, Name.to_bytes(name), wire)
        pib.set_default_identity(own_identity)
    finally:
        pib.conn.close()
    after = sorted(p.name for p in (home / ".ndn/ndnsec-key-file").glob("*.privkey"))
    if before != after or len(after) != 1:
        raise RuntimeError("PEER_PRIVATE_KEY_DISTRIBUTION")


def issue(namespace: str) -> None:
    """Create new isolated identities and reject pre-existing PIB state."""
    homes = Path("/identities")
    public = Path("/config")
    roles = ["root", "wrong-root", *ROLE_RANK]
    for role in roles:
        home = homes / role
        if (home / ".ndn").exists():
            raise ValueError("IDENTITY_REUSE:" + role)
        home.mkdir(mode=0o700, parents=True, exist_ok=True)
    def invoke(role, arguments, stdin=None):
        env = dict(os.environ, HOME=str(homes / role))
        # ndnsec's default TPM locator is now identical during setup and run.
        completed = subprocess.run(["ndnsec", *arguments], input=stdin,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=env, check=False)
        if completed.returncode:
            raise RuntimeError("NDNSEC_FAILED:" + role + ":" + arguments[0] + ":" +
                               completed.stderr.decode(errors="replace")[-800:])
        return completed.stdout
    os.umask(0o077)
    root_cert = invoke("root", ["key-gen", "-t", "r", namespace])
    (public / "root.cert").write_bytes(root_cert)
    (public / "wrong-root.cert").write_bytes(invoke(
        "wrong-root", ["key-gen", "-t", "r", namespace + "-untrusted"]))
    records = {}
    for role in ROLE_RANK:
        identity = namespace + "/" + role
        request = invoke(role, ["key-gen", "-t", "r", identity])
        request_path = homes / "root" / "request.cert"
        request_path.write_bytes(request)
        signed = invoke("root", ["cert-gen", "-s", namespace, "-i", "tiger", str(request_path)])
        cert_path = public / (role + ".cert")
        cert_path.write_bytes(signed)
        invoke(role, ["cert-install", "-f", str(cert_path)])
        installed = invoke(role, ["cert-dump", "-i", identity])
        if base64.b64decode(installed) != base64.b64decode(signed):
            raise RuntimeError("DEFAULT_CERT_MISMATCH:" + role)
        # Validators load /config/root.cert directly. Do not create an issuer
        # identity or import issuer keys into a role's PIB/TPM.
        records[role] = {"identity": identity, "certificateSha256": digest(cert_path)}
        (homes / role / "session.conf").write_text("")
    validate_role_homes({role: homes / role for role in roles})
    peer_certificates = [public / (role + ".cert") for role in ROLE_RANK] + [public / "root.cert"]
    for role in ("controller", "provider", "user", "denied"):
        install_public(homes / role, peer_certificates, namespace + "/" + role)
        # Verify with the same ndn-cxx CLI the runtime consumes, not SQL alone.
        for peer in ("controller", "provider", "user", "denied"):
            observed = invoke(role, ["cert-dump", "-i", namespace + "/" + peer])
            if base64.b64decode(observed) != base64.b64decode((public / (peer + ".cert")).read_bytes()):
                raise RuntimeError("PUBLIC_PIB_CERT_MISMATCH")
    (homes / "root" / "request.cert").unlink()
    write_json(public / "identities.json", {"root": namespace, "roles": records,
                                             "rootCertificateSha256": digest(public / "root.cert")})


if __name__ == "__main__":
    import sys
    issue(sys.argv[1])
