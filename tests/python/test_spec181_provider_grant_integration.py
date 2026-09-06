"""Real requester APP Data -> NFD -> Python/native grant runtime.

Selection metadata is a fixture input; grant transport, authority issuance,
verification and Python assembly/storage are production code. Disk faults use
a debugger-style line trace at the real disk-read boundary, not a replacement
fetch, verifier or AEAD function. This does not certify Core Selection itself.

Opt in with SPEC181_RUN_GRANT_INTEGRATION=1 and a new durable
SPEC181_GRANT_INTEGRATION_ROOT for every attempt.
"""
from dataclasses import asdict, replace
import faulthandler
import hashlib
import inspect
import json
import os
from pathlib import Path
import signal
import site
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).resolve()
EPOCH = "spec181-process-grant-v1"
PROVIDER = "/spec181/provider"
REQUESTER = "/spec181/requester"
AUTHORITY = "/spec181/authority"


def read(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    Path(path).write_text(json.dumps(value, sort_keys=True))


def digest(value):
    return "sha256:" + hashlib.sha256(value).hexdigest()


def private(path, key):
    from cryptography.hazmat.primitives import serialization
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as output:
        output.write(key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))


def setup(root, algorithm, variant):
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519, ec
    authority, requester = [ed25519.Ed25519PrivateKey.generate() for _ in range(2)]
    make_recipient = (lambda: ec.generate_private_key(ec.SECP256R1(), default_backend())) \
        if algorithm == "p256" else ed25519.Ed25519PrivateKey.generate
    private(root / "authority.pem", authority)
    private(root / "requester.pem", requester)
    private(root / "recipient.pem", make_recipient())
    private(root / "wrong-recipient.pem", make_recipient())
    contracts = root / "contracts"
    contracts.mkdir()
    public = authority.public_key().public_bytes(serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    (contracts / "authority.pub").write_bytes(public)
    write(contracts / "trust-root-registry-v1.json", dict(schemaVersion=1, status="CONFIGURED",
        artifactPolicyAuthority=dict(authorityId=AUTHORITY, keyId="fixture-authority-1",
            publicKeyAlgorithm="ed25519", signatureAlgorithm="ed25519",
            grantSchema="ndnsf-di-key-grant-v1", acceptedModelFamilies=["YOLO26n"],
            protectionEpochs=[EPOCH], publicKeyPath="contracts/authority.pub",
            publicKeySha256=digest(public))))
    write(root / "recipient-map.json", {PROVIDER: str(root / "recipient.pem")})
    vector = read(ROOT / "tests/fixtures/spec181/assembly-vectors-v1.json")["cases"][0]
    vector["role"]["protection_epoch"] = EPOCH
    write(root / "case.json", dict(vector=vector, algorithm=algorithm, variant=variant))
    (root / "canonical.onnx").write_bytes(bytes.fromhex(vector["canonicalModelHex"]))


def issue(root, publish):
    from ndnsf_distributed_inference.sdk.placement import ProviderGrantViewV1
    from ndnsf_distributed_inference.security.registry_keys import (
        load_ed25519_private_key, load_grant_recipient_private_key)
    from ndnsf_distributed_inference.security.requester_grant_pipeline import build_in_process_grant_provider
    case = read(root / "case.json")
    role = case["vector"]["role"]
    recipient = load_grant_recipient_private_key(root / (
        "wrong-recipient.pem" if case["variant"] == "wrong-recipient" else "recipient.pem"))
    content_key = os.urandom(32)
    deadline = int(time.time() * 1000) + 60000
    view = ProviderGrantViewV1(provider=PROVIDER, request_id="/spec181/process-request", attempt=1,
        plan_core_digest="sha256:" + "c" * 64, offer_digest="sha256:" + "d" * 64,
        role_digests=("sha256:" + "e" * 64,), security_policy_snapshot_digest="sha256:" + "f" * 64,
        model_manifest_digest=role["model_manifest_digest"], protection_epoch=EPOCH)
    factory = build_in_process_grant_provider(requester_identity=REQUESTER,
        requester_private_key=load_ed25519_private_key(root / "requester.pem"),
        authority_identity=AUTHORITY, authority_private_key=load_ed25519_private_key(root / "authority.pem"),
        protection_epoch=EPOCH, allowed_model_manifests=frozenset({role["model_manifest_digest"]}),
        recipient_public_keys=lambda identity: recipient.public_key() if identity == PROVIDER else None,
        content_key_owner=lambda model, epoch: content_key, publisher=publish,
        publication_identity=REQUESTER, authority_key_id="fixture-authority-1")
    binding = factory(view, deadline)
    metadata = dict(binding=asdict(binding), role=role, deadlineMs=deadline,
        planDigest="sha256:" + "a" * 64, contentKeyDigest=digest(content_key))
    write(root / "metadata.json", metadata)
    return metadata


def controller(root):
    from ndnsf import ServiceController
    policy = root / "controller.policies"
    policy.write_text("name /spec181/controller/NDNSF/ControllerPolicy/v1\n"
        "provider-policies {}\nuser-policies { user-policy {\n"
        "for /spec181/requester\nallow { /SPEC181/GRANT }\n} }\n")
    owner = ServiceController(controller_prefix="/spec181/controller",
        policy_file=str(policy), bootstrap_token_file=str(root / "bootstrap.tokens"),
        trust_schema=str(ROOT / "examples/trust-any.conf"))
    owner.start()
    try:
        certificate = subprocess.run(["ndnsec", "cert-dump", "-i", "/spec181/controller"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        (root / "controller.cert").write_bytes(certificate)
        (root / "controller-ready").touch()
        sys.stdin.readline()
    finally:
        owner.stop()


def publisher(root):
    from ndnsf import ServiceUser
    faulthandler.dump_traceback_later(10)
    token = next(line.split()[1] for line in (root / "bootstrap.tokens").read_text().splitlines()
        if line.startswith(REQUESTER + " "))
    owner = ServiceUser(group="/spec181/group", controller="/spec181/controller",
        user=REQUESTER, trust_schema=str(ROOT / "examples/trust-any.conf"),
        bootstrap_token=token, permission_wait_ms=50, handler_threads=1, ack_threads=1)
    owner.start()
    try:
        def publish(name, wire):
            result = owner.publish_signed_app_data(name, wire, freshness_ms=60000)
            if not result.success or result.error:
                raise RuntimeError("real APP Data publication failed: " + result.error)
        issue(root, publish)
        faulthandler.cancel_dump_traceback_later()
        (root / "publisher-ready").touch()
        sys.stdin.readline()
    finally:
        owner.stop()


def python_provider(root):
    import numpy as np
    import onnxruntime as ort
    from ndnsf_distributed_inference.provider import DistributedInferenceProvider
    from ndnsf_distributed_inference.artifact_deployment import ExecutionContext, ExecutionArtifactSpec
    from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec, GrantBindingV1
    from ndnsf_distributed_inference.core.protected_artifacts import (
        PlaintextLeaseRegistry, AssembledCiphertextV1, encrypt_assembled_entry)
    from ndnsf_distributed_inference.security.registry_keys import (
        load_artifact_policy_authority_registry, load_grant_recipient_private_key)
    metadata, case = read(root / "metadata.json"), read(root / "case.json")
    policy = load_artifact_policy_authority_registry(root / "contracts/trust-root-registry-v1.json",
        model_family="YOLO26n", protection_epoch=EPOCH)
    provider = DistributedInferenceProvider(None, grant_authority_public_key=policy.public_key,
        grant_authority_identity=policy.authority_id,
        grant_recipient_private_key=load_grant_recipient_private_key(root / "recipient.pem"),
        grant_fetch_timeout_ms=5000)
    role = RoleAssemblySpec(**metadata["role"])
    binding = GrantBindingV1(**metadata["binding"])
    context = SimpleNamespace(local_provider=PROVIDER, assignment=SimpleNamespace(role=role.role))
    projection = SimpleNamespace(grant_binding=binding, request_id=binding.request_id,
        attempt=binding.attempt, plan_core_digest=binding.plan_core_digest,
        security_policy_snapshot_digest=binding.security_policy_snapshot_digest,
        deadline_ms=metadata["deadlineMs"])
    leases, key = PlaintextLeaseRegistry(), None
    result = dict(status="REJECTED", boundary="BEFORE_ASSEMBLY", transport="ndn")
    original = (root / "canonical.onnx").read_bytes()
    try:
        verified = provider._verify_protected_grant(context, projection, role, leases)
        key = verified[0]
        assert digest(key) == metadata["contentKeyDigest"]
        result["grantVerified"] = True
        work = root / "work"
        work.mkdir()
        execution = ExecutionContext(spec=ExecutionArtifactSpec(role=role.role,
            backend="onnxruntime-cpu", entrypoint="", artifacts=[], metadata={}),
            artifact_paths={"model": root / "canonical.onnx"}, work_dir=work)
        execution = provider._assemble_certified_role_execution(context, execution, role,
            {role.role: {"path": str(root / "canonical.onnx")}}, _lease_registry=leases)
        result["boundary"] = "ASSEMBLED_DISK_READ"
        variant = case["variant"]
        if variant in ("tampered-ciphertext", "wrong-content-key"):
            method = provider._qualify_protected_assembly
            lines, start = inspect.getsourcelines(method)
            target = start + next(i for i, line in enumerate(lines)
                if "stored = AssembledCiphertextV1.from_bytes(cipher_path.read_bytes())" in line)
            filename = inspect.getsourcefile(method)
            def inject(frame, event, arg):
                if (event == "line" and frame.f_code.co_filename == filename
                        and frame.f_lineno == target and not result.get("diskFaultInjected")):
                    path = frame.f_locals["cipher_path"]
                    stored = AssembledCiphertextV1.from_bytes(path.read_bytes())
                    if variant == "tampered-ciphertext":
                        changed = bytearray(stored.ciphertext)
                        changed[-1] ^= 1
                        stored = replace(stored, ciphertext=bytes(changed))
                    else:
                        stored = encrypt_assembled_entry(b"z" * 32, b"fixture model bytes",
                            entry_kind=stored.entry_kind,
                            model_manifest_digest=stored.model_manifest_digest,
                            role_assembly_spec_digest=stored.role_assembly_spec_digest,
                            storage_profile_digest=stored.storage_profile_digest)
                    path.write_bytes(stored.to_bytes())
                    result["diskFaultInjected"] = True
                return inject
            sys.settrace(inject)
        try:
            execution, leases = provider._qualify_protected_assembly(context, execution, projection, role,
                _verified_grant=verified, _lease_registry=leases)
        finally:
            sys.settrace(None)
        session = ort.InferenceSession(str(execution.artifact_paths["model"]), providers=["CPUExecutionProvider"])
        output = session.run(None, {"x": np.asarray([[3.0]], dtype=np.float32)})
        assert output[0].tolist() == [[6.0]]
        del session
        result.update(status="VERIFIED", boundary="EXECUTED", output=output[0].tolist())
    except Exception as error:
        result["reason"] = str(error)
        result["exceptionType"] = type(error).__name__
    finally:
        leases.zeroize_all()
    result["zeroized"] = key is None or not any(key)
    result["plaintextRemaining"] = [str(p) for p in (root / "work").glob("*.onnx")]
    result["canonicalPreserved"] = (root / "canonical.onnx").read_bytes() == original
    write(root / "provider-result.json", result)
    return 0 if result["status"] == "VERIFIED" else 2


def namespace_case(root, backend, algorithm, variant):
    subprocess.run(["mount", "-t", "tmpfs", "-o", "mode=0755", "tmpfs", "/run"], check=True)
    Path("/run/nfd").mkdir()
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    setup(root, algorithm, variant)
    config = root / "nfd.conf"
    config.write_text("log { default_level WARN }\ntables { cs_max_packets 0 }\n"
        "face_system { unix { path /run/nfd/nfd.sock } }\n"
        'authorizations { authorize { certfile any privileges { faces "" fib "" cs "" strategy-choice "" } } }\n'
        "rib { localhost_security { trust-anchor { type any } } }\n")
    env = os.environ.copy()
    env["SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"] = str(root / "contracts/authority.pub")
    env["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"] = str(root / "recipient-map.json")
    env.pop("SPEC181_GRANT_FORWARDING_HINT", None)
    processes, files, exits = [], [], []
    def launch(name, command, *, pipe=False):
        home = root / (name + "-home")
        home.mkdir()
        child_env = dict(env, HOME=str(home),
            NDN_LOG="*=WARN:ndn_service_framework.ServiceUser=DEBUG:nac-abe.Consumer=DEBUG")
        child_env.pop("NDNSF_CONTROLLER_CERT_FILE", None)
        if name == "requester":
            child_env["NDNSF_CONTROLLER_CERT_FILE"] = str(root / "controller.cert")
        log = (root / (name + ".log")).open("w")
        files.append(log)
        process = subprocess.Popen(command, cwd=ROOT, env=child_env, stdout=log,
            stderr=subprocess.STDOUT, stdin=subprocess.PIPE if pipe else subprocess.DEVNULL, text=True)
        processes.append((name, process))
        return process
    def wait_file(path, owner):
        deadline = time.monotonic() + 15
        while not path.exists():
            if owner.poll() is not None:
                raise RuntimeError("owner exited before readiness: " + str(owner.returncode))
            if time.monotonic() >= deadline:
                raise RuntimeError("readiness timed out: " + path.name)
            time.sleep(0.02)
    try:
        nfd = launch("nfd", ["nfd", "--config", str(config)])
        wait_file(Path("/run/nfd/nfd.sock"), nfd)
        deadline = time.monotonic() + 10
        while subprocess.run(["nfdc", "status", "report"], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=3).returncode:
            if nfd.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("NFD management did not become ready")
            time.sleep(0.05)
        authority = launch("controller", [sys.executable, str(SCRIPT), "--controller", str(root)], pipe=True)
        wait_file(root / "controller-ready", authority)
        user = launch("requester", [sys.executable, str(SCRIPT), "--publisher", str(root)], pipe=True)
        wait_file(root / "publisher-ready", user)
        if backend == "native":
            command = [os.environ["SPEC181_NATIVE_GRANT_BINARY"], str(root / "metadata.json")]
        else:
            command = [sys.executable, str(SCRIPT), "--python-provider", str(root)]
        consumer = launch("provider", command)
        code = consumer.wait(timeout=20)
        if backend == "native":
            records = [line.split("SPEC181_NATIVE_GRANT_RESULT ", 1)[1] for line in
                (root / "provider.log").read_text().splitlines() if line.startswith("SPEC181_NATIVE_GRANT_RESULT ")]
            assert len(records) == 1, "native result missing"
            write(root / "provider-result.json", json.loads(records[0]))
        expected = 0 if variant == "control" else 2
        assert code == expected, "Provider exit differs from the named boundary"
        user.communicate("stop\n", timeout=10)
        assert user.returncode == 0, "requester did not exit normally"
        authority.communicate("stop\n", timeout=10)
        assert authority.returncode == 0, "controller did not exit normally"
    finally:
        for name, process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            exits.append(dict(name=name, exitStatus=process.returncode))
        for file in files:
            file.close()
        write(root / "child-exits.json", exits)


@pytest.mark.skipif(os.environ.get("SPEC181_RUN_GRANT_INTEGRATION") != "1", reason="explicit real-NFD integration")
@pytest.mark.parametrize("backend,algorithm,variant", [
    (backend, algorithm, variant) for backend in ["native", "python"]
    for algorithm in ["ed25519", "p256"] for variant in ["control", "wrong-recipient"]
] + [("python", "p256", variant) for variant in ["tampered-ciphertext", "wrong-content-key"]])
def test_real_requester_provider_grant(backend, algorithm, variant):
    base = Path(os.environ["SPEC181_GRANT_INTEGRATION_ROOT"]).resolve()
    root = base / (backend + "-" + algorithm + "-" + variant)
    root.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["PYTHONPATH"] = os.pathsep.join([str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "pythonWrapper"), str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        site.getusersitepackages(), env.get("PYTHONPATH", "")])
    with (root / "run.log").open("w") as log:
        child = subprocess.Popen(["unshare", "--user", "--map-root-user", "--net", "--mount",
            "--pid", "--fork", "--mount-proc", sys.executable, str(SCRIPT), "--namespace",
            str(root), backend, algorithm, variant], cwd=ROOT, env=env, stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True)
        try:
            assert child.wait(timeout=60) == 0, str(root / "run.log")
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
    result = read(root / "provider-result.json")
    assert result["transport"] == "ndn"
    if variant == "control":
        assert result["status"] == "VERIFIED", result
        assert result["zeroized"] in (True, "true")
    else:
        assert result["status"] == "REJECTED", result
        if backend == "native":
            assert "DI_PROTECTED_GRANT_REJECTED" in result["reason"], result
        else:
            # The Core callback adds the wire reason; this scoped test invokes
            # the production verifier/assembler and requires its typed refusal.
            assert result["exceptionType"] == "ProtectedGrantRejected", result
        if variant == "wrong-recipient":
            assert result["boundary"] == "BEFORE_ASSEMBLY"
        else:
            assert result["grantVerified"] and result["diskFaultInjected"], result
            assert "authentication" in result["reason"], result
    if backend == "python":
        assert result["zeroized"] and result["canonicalPreserved"]
        assert result["plaintextRemaining"] == []
    assert len(read(root / "child-exits.json")) == 4
    assert all(item["exitStatus"] is not None for item in read(root / "child-exits.json"))


@pytest.mark.skipif(os.environ.get("SPEC181_RUN_GRANT_SANITIZER") != "1", reason="explicit native resource diagnostic")
def test_native_p256_grant_resources():
    """Real crypto/runtime ownership check with an explicit offline wire fixture."""
    root = Path(os.environ["SPEC181_GRANT_INTEGRATION_ROOT"]).resolve() / "native-p256-resources"
    root.mkdir(parents=True, exist_ok=False)
    setup(root, "p256", "control")
    metadata = issue(root, lambda name, wire: (root / "grant-wire.json").write_bytes(wire))
    metadata["repeat"] = 10
    write(root / "metadata.json", metadata)
    env = dict(os.environ, SPEC181_GRANT_AUTHORITY_PUBLIC_KEY=str(root / "contracts/authority.pub"),
        SPEC181_PROVIDER_RECIPIENT_KEY_MAP=str(root / "recipient-map.json"),
        ASAN_OPTIONS="detect_leaks=1:halt_on_error=1")
    with (root / "asan.log").open("w") as log:
        result = subprocess.run([os.environ["SPEC181_NATIVE_GRANT_ASAN_BINARY"],
            str(root / "metadata.json"), str(root / "grant-wire.json")],
            cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    assert result.returncode == 0, str(root / "asan.log")


if __name__ == "__main__":
    action, directory, *arguments = sys.argv[1:]
    root = Path(directory)
    if action == "--namespace":
        namespace_case(root, *arguments)
    elif action == "--publisher":
        publisher(root)
    elif action == "--controller":
        controller(root)
    elif action == "--python-provider":
        raise SystemExit(python_provider(root))
    else:
        raise SystemExit("unknown worker action")
