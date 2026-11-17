#!/usr/bin/env python3
"""Run the Spec182 independent C++ authority/requester grant cases.

This launcher owns only process lifecycle and a private NFD/PIB/TPM.  The
request wire is produced and verified by the C++ probe; no Python grant oracle
or protocol implementation is used here.
"""

import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import sqlite3
import subprocess
import tempfile
import time


from spec185_native_fixture import fixture_path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = Path(os.environ.get("SPEC185_BUILD_DIR", ROOT / "build"))
CONTROLLER = "App_ServiceController"
AUTHORITY = "DI_NativeArtifactAuthority"
PROBE = "spec182-native-grant-requester-process"


def install_termination_handlers():
    def terminate(number, frame):
        raise RuntimeError("native process fixture received termination signal " + str(number))
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)


def native_program(build, name, *, source_subdir="examples", installed_subdir="bin"):
    if "NDNSF_TEST_RUNTIME_PREFIX" in os.environ:
        value = os.environ["NDNSF_TEST_RUNTIME_PREFIX"]
        if not value:
            raise ValueError("NDNSF_TEST_RUNTIME_PREFIX is empty")
        path = Path(value) / installed_subdir / name
    else:
        path = Path(build) / source_subdir / name
    if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
        raise RuntimeError("required native process executable missing: " + str(path))
    return path


def provision_actor_identities(run_root, environment, identities, runner=subprocess.run):
    """Generate one fixture root and root-issued actor certificates in the owned bootstrap PIB."""
    root = "/example/hello"
    # Certificate validity is encoded to whole seconds. Backdate this owned
    # fixture slightly so a fast consumer cannot start before the rounded start.
    not_before = (datetime.now(timezone.utc) - timedelta(minutes=1)).strftime('%Y%m%d%H%M%S')
    root_certificate = runner(["/usr/local/bin/ndnsec", "key-gen", "-t", "r", root],
                              env=environment, check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30).stdout
    write(run_root / "shared/root.cert", root_certificate, 0o644)
    for identity in identities:
        request = runner(["/usr/local/bin/ndnsec", "key-gen", "-t", "r", identity],
                         env=environment, check=True, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, timeout=30).stdout
        certificate = runner(["/usr/local/bin/ndnsec", "cert-gen", "-s", root,
                              "-i", "fixture", "--not-before", not_before, "-"],
                             input=request, env=environment,
                             check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             timeout=30).stdout
        runner(["/usr/local/bin/ndnsec", "cert-install", "-I", "-"], input=certificate,
               env=environment, check=True, stdout=subprocess.DEVNULL,
               stderr=subprocess.PIPE, timeout=30)
    return root_certificate


def certificate_name(environment, identity):
    """Read the real default certificate instead of inventing a legacy /ID-CERT suffix."""
    from ndn.encoding import Name, parse_data
    encoded = subprocess.run(["/usr/local/bin/ndnsec", "cert-dump", "-i", identity],
                             env=environment, check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=30).stdout
    name, _, _, _ = parse_data(base64.b64decode(b"".join(encoded.split()), validate=True))
    if ([bytes(value) for value in name[:-4]] !=
            [bytes(value) for value in Name.from_str(identity)]):
        raise RuntimeError("default certificate does not belong to fixture actor")
    return Name.to_str(name)


def certificate_key_name(certificate):
    """Use the key prefix from an admitted v2 certificate name, preserving typed components."""
    from ndn.encoding import Component, Name
    name = Name.from_str(certificate)
    if len(name) < 4 or bytes(name[-4]) != bytes(Component.from_str('KEY')):
        raise ValueError('fixture certificate name has no v2 key suffix')
    return Name.to_str(name[:-2])


def actor_trust_schema(identities):
    """Bind the fixed fixture's SVS signer roles to its explicitly provisioned actors."""
    roles = set()
    for identity in identities:
        role = identity.rsplit('/', 1)[-1]
        if (not identity.startswith('/example/hello/') or identity.count('/') != 3 or
                re.fullmatch(r'[A-Za-z0-9-]+', role) is None):
            raise ValueError('unsupported fixture actor identity: ' + identity)
        roles.add(role)
    if not roles:
        raise ValueError('fixture actors must not be empty')
    schema = fixture_path('native-process-trust-schema.conf').read_text()
    marker = '<user|8=user|provider|8=provider|controller|8=controller>'
    if marker not in schema:
        raise RuntimeError('fixture SVS signer policy changed; update actor mapping')
    return schema.replace(marker, '<' + '|'.join(
        value for role in sorted(roles) for value in (role, '8=' + role)) + '>')


def actor_environment(base, store):
    """Keep the actor's mutable SVS session counter beside its owned PIB/TPM."""
    store = Path(store)
    if not store.is_absolute():
        raise ValueError('fixture actor store must be absolute')
    result = dict(base)
    result.update(HOME=str(store / 'home'),
                  NDN_CLIENT_PIB='pib-sqlite3:' + str(store / 'pib'),
                  NDN_CLIENT_TPM='tpm-file:' + str(store / 'tpm'),
                  NDNSF_CONFIG=str(store / 'ndnsf-session.conf'))
    return result


def write(path: Path, data, mode=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    else:
        path.write_text(data)
    if mode is not None:
        path.chmod(mode)


def bounded_nfd_socket(run_root: Path) -> Path:
    """Choose a Unix socket path that stays below AF_UNIX pathname limits.

    Retained Spec182 run roots can be deeply nested under the repository. NFD
    rejects an overlong socket pathname before creating the socket, which is a
    fixture-startup failure rather than a native protocol result. Keep the
    normal colocated path when it is safe and use a short per-process path for
    long roots.
    """
    candidate = run_root / "nfd.sock"
    if len(os.fsencode(str(candidate))) <= 100:
        return candidate
    digest = hashlib.sha256(str(run_root).encode("utf-8")).hexdigest()[:16]
    return Path(tempfile.gettempdir()) / f"s182-nfd-{digest}-{os.getpid()}.sock"


def run(command, env, log, *, wait=True):
    handle = log.open("w", buffering=1)
    process = subprocess.Popen(command, cwd=log.parent, env=env, stdout=handle,
                               stderr=subprocess.STDOUT, text=True)
    process._ndnsf_log_handle = handle
    if wait:
        process.wait()
        handle.close()
    return process


def wait_marker(process, log, marker, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log.exists() and marker in log.read_text(errors="replace"):
            return
        status = process.poll()
        if status is not None:
            raise RuntimeError(f"process exited {status} before marker {marker}: {log}")
        time.sleep(0.05)
    raise RuntimeError(f"timeout waiting for {marker}: {log}")


def stop(process, timeout=4):
    if process is None or process.poll() is not None:
        return process.poll() if process is not None else None
    process.terminate()
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.wait(timeout=2)


def nfd_config(path, socket):
    write(path, f"""general
{{
}}
log
{{
  default_level WARN
  Forwarder INFO
}}
tables
{{
  cs_max_packets 0
}}
face_system
{{
  unix
  {{
    path {socket}
  }}
}}
authorizations
{{
  authorize
  {{
    certfile any
    privileges
    {{
      faces
      fib
      cs
      strategy-choice
    }}
  }}
}}
rib
{{
  localhost_security
  {{
    trust-anchor
    {{
      type any
    }}
  }}
}}
""")


def openssl_keys(authority_dir, requester_dir):
    authority_private = authority_dir / "authority-private.pem"
    authority_public = authority_dir / "authority-public.pem"
    requester_private = requester_dir / "requester-private.pem"
    requester_public = authority_dir / "requester-public.pem"
    for private, public in ((authority_private, authority_public),
                            (requester_private, requester_public)):
        subprocess.run(["/usr/bin/openssl", "genpkey", "-algorithm", "ED25519",
                        "-out", str(private)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(["/usr/bin/openssl", "pkey", "-in", str(private),
                        "-pubout", "-out", str(public)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        private.chmod(0o600)
        public.chmod(0o644)
    requester_public_for_authority = authority_dir / "requester-public.pem"
    # openssl generated requester_public is already in the authority directory.
    requester_public_for_authority.chmod(0o644)
    write(authority_dir / "content-key.bin", b"spec182-native-content-key-0001", 0o600)
    # The requester sees its private key and only the authority public key.
    write(requester_dir / "requester-private.pem", requester_private.read_bytes(), 0o600)
    write(requester_dir / "authority-public.pem", authority_public.read_bytes(), 0o644)
    write(requester_dir / "trust-schema.conf",
          fixture_path("native-process-trust-schema.conf").read_text(), 0o644)
    return authority_private, authority_public, requester_private


def authority_config(path, authority_dir):
    value = {
        "schema": "ndnsf-di-native-authority-v1",
        "run_for_ms": 120000,
        "permission_bootstrap_ms": 15000,
        "max_grant_ttl_ms": 60000,
        "authority": {
            "identity": "/example/hello/provider",
            "service": "/HELLO",
            "group": "/example/hello/group",
            "controller_identity": "/example/hello/controller",
            "requester_identity": "/example/hello/user",
            "protection_epoch": "epoch-1",
            "content_key_id": "model-key",
            "trust_schema_file": "trust-schema.conf",
            "authority_private_key_file": "authority-private.pem",
            "requester_public_key_file": "requester-public.pem",
            "content_key_file": "content-key.bin",
            "allowed_model_manifests": [
                "sha256:" + "1" * 64,
            ],
            "recipient_public_key_files": {
                "/example/hello/provider": "authority-public.pem",
            },
        },
    }
    write(path, json.dumps(value, indent=2) + "\n", 0o600)


def probe_config(path, mode, expect_rejection=False, target_provider=None):
    value = {
        "schema": "ndnsf-di-native-grant-process-probe-v1",
        "group": "/example/hello/group",
        "controller": "/example/hello/controller",
        "requester_identity": "/example/hello/user",
        "authority_identity": "/example/hello/provider",
        "authority_service": "/HELLO",
        "trust_schema_file": "trust-schema.conf",
        "requester_private_key_file": "requester-private.pem",
        "authority_public_key_file": "authority-public.pem",
        "protection_epoch": "epoch-1",
        "model_manifest_digest": "sha256:" + "1" * 64,
        "plan_core_digest": "sha256:" + "2" * 64,
        "grant_view_digest": "sha256:" + "3" * 64,
        "mode": mode,
        "expect_rejection": expect_rejection,
        "bootstrap_ms": 15000,
        "request_timeout_ms": 5000,
    }
    if target_provider is not None:
        value["target_provider"] = target_provider
    write(path, json.dumps(value, indent=2) + "\n", 0o600)


def decode_identity_name(value):
    """Decode the small Name encoding used by the PIB identity table."""
    data = memoryview(value)
    if len(data) < 2 or data[0] != 0x07:
        return ""
    length = data[1]
    if length != len(data) - 2:
        return ""
    offset = 2
    components = []
    while offset + 2 <= len(data):
        if data[offset] != 0x08:
            return ""
        component_length = data[offset + 1]
        offset += 2
        if offset + component_length > len(data):
            return ""
        components.append(bytes(data[offset:offset + component_length]).decode())
        offset += component_length
    return "/" + "/".join(components) if offset == len(data) else ""


def remove_private_keys_except(store, keep_identities):
    """Retain only the role's private NDN keys while preserving public certs."""
    pib = store / "pib" / "pib.db"
    with sqlite3.connect(pib) as database:
        rows = database.execute(
            "SELECT identities.identity, keys.key_name "
            "FROM identities JOIN keys ON keys.identity_id = identities.id").fetchall()
    keep = set(keep_identities)
    for identity_blob, key_name in rows:
        if decode_identity_name(identity_blob) in keep:
            continue
        private = store / "tpm" / "ndnsec-key-file" / (
            hashlib.sha256(key_name).hexdigest() + ".privkey")
        private.unlink(missing_ok=True)


def probe_libraries(probe, environment):
    """Resolve this trusted native candidate's actual ELF dependencies, without guessed SDK roots."""
    result = subprocess.run(['/usr/bin/ldd', str(probe)], env=environment,
                            capture_output=True, text=True, timeout=15)
    if result.returncode or 'not found' in result.stdout:
        raise RuntimeError('native grant probe has an unresolved ELF dependency')
    libraries = set()
    for line in result.stdout.splitlines():
        value = line.split('=>', 1)[-1].strip().split()
        if value and value[0].startswith('/'):
            path = Path(value[0])
            if not path.is_file():
                raise RuntimeError('native grant probe library missing: ' + str(path))
            libraries.add(path)
    return sorted(libraries)


def probe_command(probe, config, requester_dir, requester_store, nfd_socket, build, environment):
    # Mount only the requester-visible files, its read-only identity snapshot,
    # and the NFD Unix socket.  The authority directory and identity snapshot
    # are outside every requester mount.
    command = [
        "/usr/bin/bwrap", "--unshare-all", "--die-with-parent", "--new-session",
        "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
        "--ro-bind", "/usr", "/usr", "--ro-bind", "/bin", "/bin",
        "--ro-bind", "/sbin", "/sbin", "--ro-bind", "/lib", "/lib",
        "--ro-bind", "/lib64", "/lib64", "--ro-bind", "/etc", "/etc",
        "--ro-bind", build, build,
    ]
    # System roots are already visible. Map other resolved library files individually, keeping
    # unrelated source directories and Authority credentials outside the requester's namespace.
    system_roots = tuple(Path(name) for name in ('/usr', '/bin', '/sbin', '/lib', '/lib64'))
    for library in probe_libraries(probe, environment):
        if not any(library == root or root in library.parents for root in system_roots):
            command += ['--ro-bind', str(library), str(library)]
    command += [
        "--dir", "/run",
        "--ro-bind", requester_dir, "/run/requester",
        # The probe only reads its identity snapshot.  A read-only mount
        # prevents the isolated KeyChain from trying to reinitialize a
        # database under a different namespace path.
        "--ro-bind", requester_store, "/run/requester-store",
        "--bind", nfd_socket, "/run/nfd.sock",
        # Keep the probe's runtime locks and transient config in its private
        # tmpfs.  The requester fixture is read-only so that authority
        # private files cannot become writable through the probe namespace.
        "--setenv", "HOME", "/tmp",
        "--setenv", "NDNSF_CONFIG", "/tmp/ndnsf-session.conf",
        "--setenv", "NDN_CLIENT_PIB", "pib-sqlite3:/run/requester-store/pib",
        "--setenv", "NDN_CLIENT_TPM", "tpm-file:/run/requester-store/tpm",
        "--setenv", "NDN_CLIENT_TRANSPORT", "unix:///run/nfd.sock",
        "--chdir", "/run/requester", str(probe), "--config", "/run/requester/probe.json",
    ]
    return command


def main():
    install_termination_handlers()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--case", choices=("positive", "bad-signature", "wrong-epoch",
                                             "unknown-recipient", "malformed", "expired",
                                             "unreachable"), default="positive")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    controller = native_program(build, CONTROLLER)
    authority = native_program(build, AUTHORITY)
    probe = native_program(build, PROBE, source_subdir="", installed_subdir="libexec/ndnsf-tests")
    for executable in (controller, authority, probe):
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise RuntimeError(f"missing executable: {executable}")

    run_root = Path(tempfile.mkdtemp(prefix="spec182-r11-b1-process-", dir="/tmp"))
    run_root.chmod(0o700)
    shared = run_root / "shared"
    bootstrap_store = run_root / "bootstrap-store"
    controller_store = run_root / "controller-store"
    authority_store = run_root / "authority-store"
    requester_store = run_root / "requester-store"
    authority_dir = run_root / "authority"
    requester_dir = run_root / "requester"
    for directory in (shared, authority_dir, requester_dir):
        directory.mkdir(mode=0o700)
    (shared / "home").mkdir(mode=0o700)
    nfd_socket = run_root / "nfd.sock"
    nfd_config_path = run_root / "nfd.conf"
    policy = run_root / "hello.policies"
    nfd_log = run_root / "nfd.log"
    controller_log = run_root / "controller.log"
    authority_log = run_root / "authority.log"
    probe_log = run_root / "probe.log"
    nfd_config(nfd_config_path, nfd_socket)
    write(policy, fixture_path("controller-policies.conf").read_text(), 0o600)
    # Pre-create the NDN identities once, then give each native process a
    # private copy of the complete PIB/TPM.  Sharing a writable PIB between
    # Controller and Provider races default-identity/certificate updates in
    # ndn-cxx; the copies retain identical cert/key material without that
    # cross-process mutation.
    bootstrap_store.mkdir(mode=0o700)
    (bootstrap_store / "home").mkdir(mode=0o700)
    bootstrap_env = os.environ.copy()
    bootstrap_env.update({
        "HOME": str(bootstrap_store / "home"),
        "NDN_CLIENT_PIB": "pib-sqlite3:" + str(bootstrap_store / "pib"),
        "NDN_CLIENT_TPM": "tpm-file:" + str(bootstrap_store / "tpm"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin",
    })
    identities = (
        "/example/hello/controller", "/example/hello/provider",
        "/example/hello/provider/A", "/example/hello/provider/B",
        "/example/hello/provider/C", "/example/hello/user",
    )
    root_certificate = provision_actor_identities(run_root, bootstrap_env, identities)
    fixture_schema = actor_trust_schema(identities)
    write(shared / 'trust-schema.conf', fixture_schema, 0o644)
    for directory in (authority_dir, requester_dir):
        write(directory / "root.cert", root_certificate, 0o644)
    for store in (controller_store, authority_store, requester_store):
        shutil.copytree(bootstrap_store, store)
        # PIB stores the TPM locator as a URI.  Rewrite it after copying so
        # each process signs with the private-key files in its own snapshot,
        # rather than silently consulting the bootstrap TPM directory.
        pib = store / "pib" / "pib.db"
        with sqlite3.connect(pib) as database:
            database.execute("UPDATE tpmInfo SET tpm_locator = ?",
                             (f"tpm-file:{store / 'tpm'}",))
            database.commit()
    remove_private_keys_except(controller_store, ("/example/hello/controller",))
    remove_private_keys_except(authority_store, ("/example/hello/provider",))
    remove_private_keys_except(requester_store, ("/example/hello/user",))
    authority_private, authority_public, _ = openssl_keys(authority_dir, requester_dir)
    write(authority_dir / "trust-schema.conf", fixture_schema, 0o644)
    authority_config(authority_dir / "authority.json", authority_dir)
    probe_config(requester_dir / "probe.json", args.case, args.case != "positive",
                 "/example/hello/provider")

    env = os.environ.copy()
    env.update({
        "HOME": str(shared / "home"),
        "NDN_CLIENT_TRANSPORT": "unix://" + str(nfd_socket),
        "NDNSF_CONTROLLER_GENERATION_STATE": str(shared / "controller-generation.state"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin",
    })
    children = []
    try:
        nfd = run(["/usr/local/bin/nfd", "--config", str(nfd_config_path)], env, nfd_log,
                  wait=False)
        children.append(nfd)
        deadline = time.monotonic() + 10
        while not nfd_socket.exists():
            if nfd.poll() is not None:
                raise RuntimeError(f"NFD exited {nfd.returncode}: {nfd_log}")
            if time.monotonic() >= deadline:
                raise RuntimeError("private NFD socket startup timed out")
            time.sleep(0.05)

        controller_env = actor_environment(env, controller_store)
        controller_process = run([
            str(controller), "--controller-prefix", "/example/hello/controller",
            "--trust-schema", str(shared / 'trust-schema.conf'),
            "--policy-file", str(policy), "--ensure-identities",
            "/example/hello/provider,/example/hello/user", "--no-serve-certificates",
            "--run-for-ms", "120000",
        ], controller_env, controller_log, wait=False)
        children.append(controller_process)
        wait_marker(controller_process, controller_log, "ServiceController started...", 20)

        authority_env = actor_environment(env, authority_store)
        authority_process = run([str(authority), "--config", str(authority_dir / "authority.json")],
                                authority_env, authority_log, wait=False)
        children.append(authority_process)
        wait_marker(authority_process, authority_log, "NATIVE_GRANT_AUTHORITY_READY", 30)

        if args.case == "unreachable":
            stop(authority_process)
            children.remove(authority_process)

        config = requester_dir / "probe.json"
        command = probe_command(probe, config, requester_dir, requester_store,
                                nfd_socket, build, env)
        probe_process = run(command, env, probe_log, wait=True)
        output = probe_log.read_text(errors="replace")
        print("SPEC182_NATIVE_GRANT_PROCESS_ROOT=" + str(run_root))
        print(f"SPEC182_NATIVE_GRANT_PROCESS_CASE={args.case}")
        print(f"SPEC182_NATIVE_GRANT_PROCESS_RC={probe_process.returncode}")
        for line in output.splitlines():
            if line.startswith(("NATIVE_GRANT_REQUESTER_", "NATIVE_GRANT_PROCESS_")):
                print(line)
        if probe_process.returncode != 0:
            raise RuntimeError(f"C++ probe failed; see {probe_log}")
        expected = "NATIVE_GRANT_PROCESS_POSITIVE" if args.case == "positive" else "NATIVE_GRANT_PROCESS_REJECTED"
        if expected not in output:
            raise RuntimeError(f"missing expected marker {expected}; see {probe_log}")
        # The requester-visible mount contains no authority private/content
        # key names or bytes.  Check the actual isolated process inputs.
        visible = "\n".join(p.name for p in requester_dir.iterdir())
        if "authority-private.pem" in visible or "content-key.bin" in visible:
            raise RuntimeError("requester-visible directory contains authority secret material")
        print("SPEC182_NATIVE_GRANT_PROCESS_ISOLATION=PASS")
        return 0
    finally:
        for child in reversed(children):
            stop(child)
        if args.keep:
            print("SPEC182_NATIVE_GRANT_PROCESS_ARTIFACT_DIR=" + str(run_root))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
