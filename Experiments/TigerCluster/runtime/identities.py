"""Offline per-run issuer using the exact SIF's ndnsec implementation.

Each role keeps only its own private key. The issuer handles certificate
requests and returns public certificates, without exporting its root key.
"""
from __future__ import annotations

import base64
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess

from runtime.baseline import ROLE_RANK, write_json, digest


class RoleHomeLease:
    """Cooperating worker exclusion for one prepared HOME, held until teardown.

    This is not an authorization credential or a lock understood by ndn-cxx.
    Allocation/process ownership still handles worker crashes and uncooperative
    applications; the run coordinator must not reuse private run directories.
    """
    def __init__(self, home: Path):
        self.fd = None
        home = Path(home)
        validate_role_homes({home.name: home})
        flags = os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK
        fd = os.open(str(home / ".ndn/.runtime.lock"), flags, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError("ROLE_LEASE_FILE")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise ValueError("ROLE_HOME_IN_USE:" + home.name) from exc
        except BaseException:
            os.close(fd)
            raise
        self.fd = fd

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


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


def identity_inventory(namespace: str, role_identities: dict[str, str] | None = None) -> dict[str, str]:
    """Resolve a run's plain-component identity names before invoking ndnsec.

    Experiment-generated names intentionally exclude URI escapes/typed-name
    aliases, so two role strings cannot identify the same PIB identity.
    This is an experiment naming restriction, not an NDNSF protocol rule.
    """
    name_pattern = r"/(?:[A-Za-z0-9_-][A-Za-z0-9_.-]*)(?:/[A-Za-z0-9_-][A-Za-z0-9_.-]*)*"
    if not isinstance(namespace, str) or not re.fullmatch(name_pattern, namespace):
        raise ValueError("IDENTITY_NAMESPACE")
    identities = ({role: namespace + "/" + role for role in ROLE_RANK}
                  if role_identities is None else role_identities)
    if not isinstance(identities, dict) or not identities:
        raise ValueError("IDENTITY_INVENTORY")
    seen = set()
    for role, identity in identities.items():
        if (not isinstance(role, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", role)
                or role in ("root", "wrong-root")):
            raise ValueError("IDENTITY_ROLE")
        if (not isinstance(identity, str) or not re.fullmatch(name_pattern, identity)
                or not identity.startswith(namespace + "/") or identity in seen):
            raise ValueError("IDENTITY_NAME:" + role)
        seen.add(identity)
    return dict(identities)


def issue(namespace: str, role_identities: dict[str, str] | None = None) -> None:
    """Create new isolated identities and reject pre-existing PIB state."""
    identity_map = identity_inventory(namespace, role_identities)
    peer_roles = (tuple(identity_map) if role_identities is not None else
                  ("controller", "provider", "user", "denied"))
    homes = Path("/identities")
    public = Path("/config")
    roles = ["root", "wrong-root", *identity_map]
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
                                   env=env, check=False, timeout=15)
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
    for role, identity in identity_map.items():
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
    peer_certificates = [public / (role + ".cert") for role in identity_map] + [public / "root.cert"]
    for role in peer_roles:
        install_public(homes / role, peer_certificates, identity_map[role])
        # Verify with the same ndn-cxx CLI the runtime consumes, not SQL alone.
        for peer in peer_roles:
            observed = invoke(role, ["cert-dump", "-i", identity_map[peer]])
            if base64.b64decode(observed) != base64.b64decode((public / (peer + ".cert")).read_bytes()):
                raise RuntimeError("PUBLIC_PIB_CERT_MISMATCH")
    (homes / "root" / "request.cert").unlink()
    write_json(public / "identities.json", {"root": namespace, "roles": records,
                                             "rootCertificateSha256": digest(public / "root.cert")})


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("namespace")
    parser.add_argument("--roles-json", type=Path,
                        help="Derived role-to-identity map from the frozen run plan")
    args = parser.parse_args()
    issue(args.namespace, json.loads(args.roles_json.read_text()) if args.roles_json else None)
