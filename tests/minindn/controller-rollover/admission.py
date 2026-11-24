"""Evidence for cold service refusal and recipient-specific Grant rejection."""

import json
import re
from decimal import Decimal
from pathlib import Path

from withdrawal import committed_revocation, denied_actor_exits, records, require


DIGEST = re.compile(r"sha256:[0-9a-f]{64}")
GRANT_REJECTION = "DI_PROTECTED_GRANT_REJECTED: content-key envelope failed authentication"
MARKERS = ("NATIVE_CONVERSATION_TURN_START ", "NDNSF_DI_REQUEST_TERMINAL ",
           "NDNSF_DI_NATIVE_SELECTION_COMMITTED ", "NDNSF_DI_NATIVE_SELECTION_ACCEPTED ",
           "NDNSF_DI_GRANT_VERIFICATION ", "NDNSF_DI_PROVIDER_STAGE ",
           "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED ", "NDNSF_DI_GRANT_VERIFIED ",
           "NDNSF_CONTROLLER_REVOKED ", "NDNSF_REVOCATION_APPLIED ", "Installed PolicyStatus ",
           "NDNSF_POLICY_STATUS_PUBLISHED ", "SPEC220_PREP_STAGE requester.user-prepare.complete")


def actor_log(evidence, actor):
    with (evidence / (actor["id"] + ".log")).open(errors="replace") as stream:
        return [line for line in stream if any(marker in line for marker in MARKERS)]


def json_rows(lines, marker):
    return [json.loads(line.split(marker + " ", 1)[1]) for line in lines if marker + " " in line]


def configure(root, config, case):
    controller = next(a for a in config["actors"] if a["kind"] == "controller")
    if case == "user-withdrawal":
        service = config["network"]["service"]
        controller["parameters"].update({"revoke-service": service, "revoke-attribute": "/PERMISSION" + service})
        return {}
    require(case == "invalid-grant", "unknown admission case")
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    role, provider = next(iter(config["requester"]["fixed_placement"]["role_providers"].items()))
    mapping = config["artifact_authority"]["authority"]["recipient_public_key_files"]
    original = Path(mapping[provider]).resolve(strict=True)
    actor = next(a for a in config["actors"] if a["identity"] == provider and a["kind"] == "provider")
    key_map = Path(actor["parameters"]["recipient-key-map"]).resolve(strict=True)
    require(root in original.parents and root in key_map.parents, "recipient key inputs are outside the fixture")
    private_path = Path(json.loads(key_map.read_text())[provider]).resolve(strict=True)
    require(root in private_path.parents, "recipient key is outside the fixture")
    key = serialization.load_pem_private_key(private_path.read_bytes(), password=None)
    require(isinstance(key, Ed25519PrivateKey), "expected a fixture-owned Ed25519 recipient")
    encoding = serialization.Encoding.PEM
    format_ = serialization.PublicFormat.SubjectPublicKeyInfo
    original_public = serialization.load_pem_public_key(original.read_bytes()).public_bytes(encoding, format_)
    require(original_public == key.public_key().public_bytes(encoding, format_),
            "unmodified Authority mapping already differs from target recipient")
    substitute = Ed25519PrivateKey.generate().public_key().public_bytes(
        encoding, format_)
    substitute_path = root / "invalid-recipient-public.pem"
    require(substitute != original_public, "recipient substitution is ineffective")
    with substitute_path.open("xb") as stream:
        stream.write(substitute)
    mapping[provider] = str(substitute_path)
    return {"provider": provider, "role": role, "originalPublicKey": str(original),
            "submittedPublicKey": str(substitute_path)}


def grant_rejection(config, evidence, requester, terminal, request_id, target):
    provider = next(a for a in config["actors"] if a["identity"] == target["provider"])
    rows = actor_log(evidence, provider)
    bindings = [row for _, row in records(rows, "NDNSF_DI_NATIVE_SELECTION_ACCEPTED")]
    require(len(bindings) == 1, "target did not accept exactly one real Selection")
    binding = bindings[0]
    require(binding.get("requestId") == request_id and binding.get("role") == target["role"]
            and binding.get("provider") == target["provider"] and DIGEST.fullmatch(binding["planDigest"])
            and int(binding["attemptEpoch"]) > 0, "target Selection binding mismatch")
    commits = [row for _, row in records(requester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED")]
    require(len(commits) == 1 and all(commits[0].get(k) == binding[k] for k in
            ("requestId", "planDigest", "attemptEpoch")), "Requester did not commit target Selection")
    verifications = json_rows(rows, "NDNSF_DI_GRANT_VERIFICATION")
    require(len(verifications) == 1, "missing or duplicate Provider Grant verification")
    rejection = verifications[0]
    require(rejection.get("status") == "REJECTED" and rejection.get("boundary") == "BEFORE_ASSEMBLY"
            and rejection.get("reason") == GRANT_REJECTION
            and all(rejection.get(k) == binding[k] for k in ("provider", "requestId", "planDigest"))
            and rejection.get("attemptId") == "attempt-" + binding["attemptEpoch"]
            and DIGEST.fullmatch(rejection["grantDigest"]) and DIGEST.fullmatch(rejection["planCoreDigest"]),
            "wrong target, Grant, attempt or rejection boundary")
    # This isolated Provider has one selected role. A later attempt or a stage
    # lacking an ID cannot conceal unexpected assembly or backend execution.
    stages = [row for _, row in records(rows, "NDNSF_DI_PROVIDER_STAGE")]
    require(all(row.get("stage") in {"GRANT_REJECTED", "TERMINAL"} and row.get("status") == "failed"
                for row in stages) and not any("NDNSF_DI_GRANT_VERIFIED " in line or
                "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED " in line for line in rows),
            "target continued past Grant rejection")
    require(terminal["code"] == "NATIVE_STREAM_FAILED" and terminal["boundary"] == "stream"
            and terminal["attemptEpoch"] == int(binding["attemptEpoch"]),
            "Requester failed outside the selected stream attempt")
    return {"grantVerification": rejection, "role": target["role"]}


def verify(config, evidence, case, target, trigger_ns):
    operation = json.loads((evidence / "operation.json").read_text())
    user = next(a for a in config["actors"] if a["kind"] == "requester")
    controller = next(a for a in config["actors"] if a["kind"] == "controller")
    exits = denied_actor_exits(config, operation, user["id"])
    controller_rows = actor_log(evidence, controller)
    scope = controller["parameters"]["revoke-service"]
    require(controller["parameters"]["revoke-identity"] == user["identity"] and
            controller["parameters"]["revoke-attribute"] == "/PERMISSION" + scope,
            "incorrect User revocation target")
    generation, epoch, committed = committed_revocation(controller_rows, user["identity"], scope, trigger_ns)
    started = int(Decimal(str(operation["requester"]["started_unix_seconds"])) * 1_000_000_000)
    require(committed <= started, "revocation had not settled when the cold Requester started")
    service = config["network"]["service"]
    version = dict(service=service, generation=generation, epoch=epoch)
    require(any(all(row.get(k) == v for k, v in version.items()) for _, row in
                records(controller_rows, "NDNSF_POLICY_STATUS_PUBLISHED")), "missing target signed status publication")
    requester = actor_log(evidence, user)
    terminals = json_rows(requester, "NDNSF_DI_REQUEST_TERMINAL")
    require(len(terminals) == 1, "expected one structured Requester terminal")
    terminal = terminals[0]
    turns = [row for _, row in records(requester, "NATIVE_CONVERSATION_TURN_START")]
    require(len(turns) == 1 and turns[0].get("index") == "0" and terminal.get("requestId") and
            turns[0].get("request") == terminal["requestId"] and type(terminal.get("attemptEpoch")) is int
            and terminal["attemptEpoch"] > 0 and type(terminal.get("epochMs")) is int
            and terminal["epochMs"] >= started // 1_000_000,
            "Requester terminal does not match first native turn")
    require(any("SPEC220_PREP_STAGE requester.user-prepare.complete" in line for line in requester),
            "stored model preparation did not finish")
    require(all(not Path(turn[field]).exists() for turn in config["requester"]["turns"]
                for field in ("output_file", "checkpoint_output_file")), "denied run wrote output or checkpoint")
    if case == "user-withdrawal":
        require(scope == service and terminal["code"] == "NATIVE_REQUEST_AUTHORITY_WITHDRAWN" and
                terminal["boundary"] == "bootstrap" and
                terminal["message"] == "Controller withdrew Requester authority for " + service,
                "Requester did not reject primary-service authority before discovery")
        require(any(stamp is not None and started <= stamp and stamp // 1_000_000 <= terminal["epochMs"]
                    and all(row.get(k) == v for k, v in version.items()) for stamp, row in
                    records(requester, "Installed PolicyStatus")), "missing authenticated Requester target status")
        require(not list(records(requester, "NDNSF_DI_NATIVE_SELECTION_COMMITTED")),
                "Requester committed Selection after withdrawal")
        for provider in (a for a in config["actors"] if a["kind"] == "provider"):
            require(not any("NDNSF_DI_" in line for line in actor_log(evidence, provider)),
                    "cold refusal reached a Provider Selection, Grant or backend stage")
        receipt = {}
    else:
        require(case == "invalid-grant" and scope != service, "invalid-Grant control revoked inference")
        receipt = grant_rejection(config, evidence, requester, terminal, terminal["requestId"], target)
    return dict(receipt, service=service, revokedScope=scope, generation=generation, epoch=epoch,
                committedNs=committed, requestTerminal=terminal, actorExitCodes=exits, evidence=str(evidence))
