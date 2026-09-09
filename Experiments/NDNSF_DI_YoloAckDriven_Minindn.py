#!/usr/bin/env python3
"""Source-bound entry point for the Spec180 YOLO MiniNDN qualification.

The expensive MiniNDN execution is deliberately behind a strict preflight.
This module owns the candidate/input/evidence boundary; it must never turn a
synthetic fixture or the legacy deployment-first YOLO runner into qualification
evidence.  ``run_minindn_case`` owns the barriered NFD/SVS startup path, but it
must fail closed until the signed package and candidate-bound profile are
available.
"""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import shutil
import site
import sys
import subprocess
import time
from typing import Any, Mapping


ROOT = Path(__file__).resolve().parents[1]
# The publication/manifest path imports the repository client before child
# processes are spawned.  Keep the launcher import boundary identical to the
# child PYTHONPATH below; otherwise a real entrypoint run fails before MiniNDN
# starts even though isolated module tests pass.
_LOCAL_PYTHON_ROOTS = (
    ROOT / "NDNSF-DistributedRepo/pythonWrapper",
    ROOT / "pythonWrapper",
    ROOT / "NDNSF-DistributedInference",
    ROOT,  # Shared pure evidence validators used by MiniNDN and Tiger jobs.
)
for _python_root in reversed(_LOCAL_PYTHON_ROOTS):
    if _python_root.is_dir() and str(_python_root) not in sys.path:
        sys.path.insert(0, str(_python_root))

from Experiments import minindn_network_resources as network_resources

# Exact-SIF replay contract (SPEC180_RUNTIME_SIF / SPEC180_RUNTIME_APPTAINER):
# when the environment declares the sealed candidate image, every NFD and
# application child command is prefixed with the only approved Apptainer
# command provider.  --cleanenv prevents host Python/ABI state from leaking
# into the candidate; the host MiniNDN namespace remains the outer process
# context while Apptainer supplies the sealed application runtime inside it.
# A host-process fallback marker guards against a silent command-provider
# regression in the replay driver.
SIF_RUNTIME_SIF = os.environ.get("SPEC180_RUNTIME_SIF", "")
SIF_RUNTIME_APPTAINER = os.environ.get(
    "SPEC180_RUNTIME_APPTAINER", "/opt/apptainer/1.5.3/bin/apptainer")
# The outer launch owner verifies the base/application content identities.
# This generic command provider only maps that selected immutable application.
SIF_RUNTIME_APP_ROOT = os.environ.get("SPEC180_RUNTIME_APP_ROOT", "")
SIF_RUNTIME_REPO = "/app/repo" if SIF_RUNTIME_APP_ROOT else "/opt/ndnsf-di/replay/repo"
SIF_RUNTIME_BIN = "/app/bin" if SIF_RUNTIME_APP_ROOT else "/opt/ndnsf-di/current/bin"
SIF_RUNTIME_PYTHON = "/opt/venv/bin/python"
SIF_RUNTIME_PYTHONPATH = ":".join((
    "/opt/venv/lib/python3.10/site-packages",
    f"{SIF_RUNTIME_REPO}/NDNSF-DistributedInference",
    f"{SIF_RUNTIME_REPO}/NDNSF-DistributedRepo/pythonWrapper",
    f"{SIF_RUNTIME_REPO}/pythonWrapper",
    f"{SIF_RUNTIME_REPO}/examples/python",
))
SPEC180_SIF_HOST_PROCESS_FALLBACK = "SPEC180_SIF_HOST_PROCESS_FALLBACK"

# MiniNDN's legacy ``getPopen(..., shell=True)`` implementation reads the
# launcher's process-wide SHELL variable instead of the per-node envDict.  The
# systemd system manager intentionally supplies a minimal environment, so set
# the deterministic shell used by the command provider before any child starts.
if not os.environ.get("SHELL"):
    os.environ["SHELL"] = "/bin/bash"


def sif_runtime_enabled() -> bool:
    """True only when the sealed candidate image is declared."""
    return bool(SIF_RUNTIME_SIF)


def _sif_bind_args(base_env: Mapping[str, str] | None = None) -> list[str]:
    """Return data/control-plane bind mounts for the candidate image.

    A selected external application is mounted read-only at /app; foundational
    libraries remain inside the base image. The outer owner verifies identities.
    """
    if not sif_runtime_enabled():
        return []
    # Under sudo, Path.home() resolves to /root; the operator's secret/data
    # trees live under the invoking user's home, so resolve that explicitly.
    operator_home = Path.home()
    sudo_user = os.environ.get("SUDO_USER", "")
    if sudo_user and os.geteuid() == 0 and (Path("/home") / sudo_user).is_dir():
        operator_home = Path("/home") / sudo_user
    environment = os.environ if base_env is None else base_env
    bind_roots = [
        ROOT / "results",
        # Spec183 run outputs live under the canonical TigerCluster owner;
        # bind that tree explicitly because the repository root has no
        # top-level results directory in this checkout.
        ROOT / "Experiments/TigerCluster/results",
        ROOT / "specs",
        Path("/run/nfd"),
        operator_home / ".local/state/ndnsf/spec180",
        operator_home / ".config/ndnsf/spec180",
    ]
    result: list[str] = []
    # Generated trust schemas use the stable in-container anchor path
    # ``/config/root.cert``.  The exact-SIF replay otherwise binds the run
    # tree only at its host absolute path, so ValidatorConfig sees a missing
    # anchor and reports the misleading "policy did not invoke" error.  Bind
    # the candidate's public directory explicitly for every application
    # child; NFD receives an empty base environment and does not need it.
    trust_root_value = str(environment.get(
        "SPEC180_YOLO_OFFER_TRUST_ROOT", "") or "").strip()
    if trust_root_value:
        trust_root = Path(trust_root_value).expanduser().resolve()
        if (not trust_root.is_file()
                or any(p.is_symlink() for p in (trust_root, *trust_root.parents))):
            raise RunnerError("SIF_TRUST_ROOT_PATH_INVALID")
        public_dir = trust_root.parent
        result.extend(["--bind", f"{public_dir}:/config:ro"])
    if SIF_RUNTIME_APP_ROOT:
        app = Path(SIF_RUNTIME_APP_ROOT)
        if (not app.is_absolute() or '..' in app.parts or not app.is_dir()
                or any(p.is_symlink() for p in (app, *app.parents))
                or not re.fullmatch(r'[A-Za-z0-9_./-]+', str(app))):
            raise RunnerError('SIF_APPLICATION_PATH_INVALID')
        result.extend(['--bind', f'{app}:/app:ro'])
    # The canonical model package is an external, immutable input.  It lives
    # outside the repository's results/specs trees and therefore needs an
    # explicit read-only bind when the MiniNDN process runs inside the exact
    # SIF.  Passing its host path as an environment variable without this bind
    # makes package verification fail before NFD starts.
    package_value = os.environ.get("SPEC180_YOLO_CANONICAL_PACKAGE", "").strip()
    if package_value:
        package = Path(package_value).expanduser().resolve()
        if (not package.is_absolute() or not package.is_dir()
                or any(p.is_symlink() for p in (package, *package.parents))):
            raise RunnerError("SIF_CANONICAL_PACKAGE_PATH_INVALID")
        result.extend(["--bind", f"{package}:{package}:ro"])
    seen: set[str] = set()
    for raw in bind_roots:
        path = Path(raw).expanduser().resolve()
        if not path.exists() or str(path) in seen:
            continue
        seen.add(str(path))
        result.extend(["--bind", f"{path}:{path}"])
    return result


def sif_exec_prefix(base_env: Mapping[str, str] | None = None,
                    *, home_dir: str | None = None) -> str:
    """Return the only approved application command prefix for exact-SIF replay.

    Apptainer 1.5.3 rejects ``--env HOME=...``; use ``--home`` so every node
    gets its own PIB, TPM, client.conf, and NFD management socket.
    """
    if not sif_runtime_enabled():
        return ""
    # NFD's MiniNDN configuration uses the conventional /run/nfd/<node>.sock
    # path.  The sealed image has a read-only /run, so create and explicitly
    # bind the host socket directory before any SIF child starts.  The host
    # systemd owner is root for this operation; unprivileged callers fail
    # closed instead of silently falling back to a host socket.
    nfd_socket_root = Path("/run/nfd")
    if not nfd_socket_root.exists():
        if os.geteuid() != 0:
            raise RunnerError("SIF_NFD_SOCKET_ROOT_UNAVAILABLE")
        nfd_socket_root.mkdir(mode=0o755, parents=True, exist_ok=True)
    if nfd_socket_root.is_symlink() or not nfd_socket_root.is_dir():
        raise RunnerError("SIF_NFD_SOCKET_ROOT_INVALID")
    env = base_env or {}
    pieces = [
        shutil.which(SIF_RUNTIME_APPTAINER) or SIF_RUNTIME_APPTAINER,
        "exec", "--cleanenv",
        *_sif_bind_args(base_env),
    ]
    if home_dir:
        selected_home_path = Path(home_dir).expanduser().resolve()
        pieces.extend(["--home", f"{selected_home_path}:{selected_home_path}"])
    else:
        pieces.extend(["--home", '"${HOME:-/tmp/minindn}:${HOME:-/tmp/minindn}"'])
    pieces.extend([
        "--pwd", SIF_RUNTIME_REPO,
        "--env", "PATH=/opt/venv/bin:/opt/ndnsf-di/current/bin:/usr/local/bin:/usr/bin:/bin",
        "--env", "LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib",
        "--env", f"PYTHONPATH={SIF_RUNTIME_PYTHONPATH}",
        "--env", "PYTHONNOUSERSITE=1",
    ])
    for key, value in sorted(env.items()):
        if not (key.startswith("NDNSF_") or key.startswith("SPEC180_")
                or key.startswith("SPEC181_")
                or key == "NDN_LOG"):
            continue
        if key in {"SPEC180_RUNTIME_SIF", "SPEC180_RUNTIME_APPTAINER",
                   "SPEC180_RUNTIME_APP_ROOT"}:
            continue
        pieces.extend(["--env", f"{key}={value}"])
    pieces.extend([
        "--env", 'NDN_CLIENT_CONF="${NDN_CLIENT_CONF:-}"',
        "--env", 'NDN_CLIENT_TRANSPORT="${NDN_CLIENT_TRANSPORT:-}"',
        SIF_RUNTIME_SIF,
    ])
    return " ".join(pieces)


def sif_python_prefix(base_env: Mapping[str, str], *, home_dir: str | None = None) -> str:
    """Fail-closed Python command prefix for MiniNDN node processes."""
    if sif_runtime_enabled():
        return (sif_exec_prefix(base_env, home_dir=home_dir)
                + " " + SIF_RUNTIME_PYTHON + " ")
    return ""


class Spec180SifNfd:
    """NFD application wrapper for the host-orchestrated exact-SIF replay.

    Built lazily around the legacy MiniNDN Nfd class: when the SIF contract is
    active, NFD starts through the Apptainer command provider so the sealed
    candidate image owns the forwarder binary too.
    """

    def __new__(cls, *args, **kwargs):
        legacy = importlib.import_module("NDNSF_DI_Yolo2x2_Minindn")
        base = legacy.Nfd

        class SifNfd(base):
            def start(self):  # noqa: D401 - MiniNDN Application API
                if not sif_runtime_enabled():
                    return super().start()
                command = (
                    "exec " + sif_exec_prefix({}, home_dir=self.homeDir)
                    + " nfd --config " + self.confFile
                )
                # Application.start splits string commands, which would
                # destroy the quoted Apptainer command.  Pass a shell argv
                # instead so MiniNDN keeps its normal process ownership.
                import minindn.apps.application as _application
                _application.Application.start(
                    self, ["bash", "-lc", command], logfile=self.logFile)

        return SifNfd(*args, **kwargs)

REQUIRED_ENV = (
    "NDNSF_DI_STATE_ROOT",
    "NDNSF_DI_ENVELOPE_KEY_FILE",
    "SPEC180_YOLO_CANONICAL_PACKAGE",
    "SPEC180_YOLO_CATALOGUE_REGISTRY",
    "SPEC180_YOLO_CATALOG_DATA_NAME",
    "SPEC180_YOLO_CATALOG_SIGNER",
    "SPEC180_YOLO_OFFER_TRUST_ROOT",
    "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP",
    "SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP",
    "SPEC180_YOLO_TOPOLOGY",
    "SPEC180_YOLO_CONFIG",
)


def _child_process_environment(base: Mapping[str, str]) -> dict[str, str]:
    """Return the controlled application environment for one case.

    ``NDN_LOG`` on the runner is intentionally not inherited.  Operators use
    ``SPEC180_CHILD_NDN_LOG`` when they need Controller/Repo/Provider/User
    diagnostics without also changing NFD logging during network startup.
    """
    env = dict(base)
    child_ndn_log = env.pop("SPEC180_CHILD_NDN_LOG", "").strip() or "*=WARN"
    env.pop("NDN_LOG", None)
    # MiniNDN's getPopen() supplies the node-scoped HOME after collecting the
    # node environment.  Do not let the runner's HOME overwrite that value:
    # otherwise Controller/User/Provider share the operator PIB/TPM, and a
    # real child can select a key that was never installed in its node.
    env.pop("HOME", None)
    # This topology installs requester identity routes before child startup.
    # A router-name hint can divert exact grant Interests from those routes.
    env.pop("SPEC181_GRANT_FORWARDING_HINT", None)
    env["NDN_LOG"] = child_ndn_log
    return env
CASE_IDS = ("Y-A", "Y-B", "Y-N")
YN_SUBCASES = ("Y-N-O", "Y-N-C", "Y-N-P", "Y-N-R", "Y-N-I", "Y-N-E", "Y-N-L")
YN_SUBCASE_BOUNDARIES = {
    "Y-N-O": "TERMINAL_RESPONSE",
    "Y-N-C": "PLACEMENT_DECISION",
    "Y-N-P": "ACK_CLOSED",
    "Y-N-R": "PLAN_SEALED",
    "Y-N-I": "PROVIDER_EXECUTION_STARTED",
    "Y-N-E": "PROVIDER_GRANT_VERIFICATION",
    "Y-N-L": "EVIDENCE_ACCEPTANCE",
}
# Y-N-E requires a selected Provider verifier record; User-local probes
# and synthetic PROTECTION_EPOCH_REJECTED markers are not evidence.
YN_NEGATIVE_REASONS = {
    "Y-N-C": "NO_FEASIBLE_CANDIDATE",
    "Y-N-P": "ACK_PROVENANCE_REJECTED",
    "Y-N-R": "ROLE_KIND_REJECTED",
    "Y-N-I": "NON_INGRESS_INPUT_REJECTED",
    # spec181 T006: real grant mutations reach the implemented verifier and
    # are rejected at the authorization boundary (before assembly).
    "Y-N-E": "DI_PROTECTED_GRANT_REJECTED",
    "Y-N-L": "REDACTION_REJECTED",
}
# Expected User exit code in Y-N runs. This exit alone does not prove PASS;
# Y-N-E additionally requires the selected Provider verifier evidence.
YN_NEGATIVE_PASS_EXIT = 91
YN_GRANT_REJECTIONS = {
    "EXPIRED": "DI_PROTECTED_GRANT_REJECTED: key grant is expired",
    "WRONG_RECIPIENT": "DI_PROTECTED_GRANT_REJECTED: content-key envelope failed authentication",
    "FORGED_AUTHORITY": "DI_PROTECTED_GRANT_REJECTED: key grant authority signature is invalid",
}

# spec181 R004 honesty gate: until grant wiring lands (T001/T002), a request
# for protected-epoch execution (any role bound to a real protection epoch
# such as the fixed `spec180-yolo-protected-v1`) must fail closed with
# DI_PROTECTED_GRANT_UNAVAILABLE instead of running the plaintext path and
# emitting a PASS record.  The requester declares the epoch on the same env
# channel the runner already uses for subcase routing (SPEC180_YN_MUTATION).
# spec181 T001/T002 landed (Python provider grant qualification + native
# verifier with parity lock); the R004 gate is absorbed.
GRANT_WIRING_AVAILABLE = True
PROTECTION_EPOCH_ENV = "SPEC181_PROTECTION_EPOCH"
STATE_ROOT_OWNER_ENV = "NDNSF_DI_STATE_ROOT_OWNER_UID"
PLAINTEXT_EPOCH = "plaintext-v1"


def _assert_grant_wiring_or_plaintext(environment: Mapping[str, str]) -> None:
    """Refuse protected-epoch execution before grant wiring exists.

    Pre-T001/T002 no verifier can authenticate a grant, so an execution that
    asks for a non-plaintext protection epoch must not silently fall back to
    the plaintext path (spec181 R004).  The check runs before any output
    root, MiniNDN, or child-process side effect; plaintext-v1 requests and
    requests without the env declaration are unaffected.
    """
    requested = str(environment.get(PROTECTION_EPOCH_ENV, "") or "").strip()
    if not requested or requested == PLAINTEXT_EPOCH:
        return
    if GRANT_WIRING_AVAILABLE:
        return  # T001/T002 landed; the protected request proceeds to the verifier.
    raise RunnerError(
        "DI_PROTECTED_GRANT_UNAVAILABLE: protected-epoch execution requested"
        " (" + requested + ") but grant wiring is not implemented (spec181"
        " R004; T001/T002) - refusing to run the plaintext path as a"
        " protected epoch")

CASE_CANDIDATES = {
    "Y-A": ("atomic-v1",),
    "Y-B": ("shared-backbone-two-shard-v1",),
    "Y-N": ("atomic-v1", "shared-backbone-two-shard-v1"),
}
CASE_ROLE_SETS = {
    "atomic-v1": ("FullModel",),
    "shared-backbone-two-shard-v1": (
        "BackboneNeck", "DetectShard0", "DetectShard1", "Merge",
    ),
}
CASE_CONFIG_ROLE_SETS = {
    "Y-A": {"FullModel"},
    "Y-B": {"BackboneNeck", "DetectShard0", "DetectShard1", "Merge"},
    "Y-N": {"FullModel", "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"},
}
# These are fixed capability-profile sizes, not request-time assignments.  The
# live ACK snapshot still chooses the Provider for each role.  The cardinality
# check prevents a nominal Y-B/Y-N policy with one all-capable Provider from
# silently turning the multi-Provider case into a single-process test.
CASE_PROVIDER_COUNTS = {"Y-A": 1, "Y-B": 4, "Y-N": 4}
STARTUP_PHASES = ("control", "providers", "user")
MILESTONES = (
    "INPUT_REFERENCE_PUBLISHED",
    "REQUEST_SENT",
    "ACK_CLOSED",
    "GRAPH_READY",
    "PLACEMENT_DECISION",
    "ARTIFACTS_READY",
    "PLAN_SEALED",
    "SELECTION_COMMITTED",
    "PROVIDER_EXECUTION_STARTED",
    "TERMINAL_RESPONSE",
)
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_NAME_RE = re.compile(r"^/[^\s]+$")
_IDENTITY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")
_SECRET_RE = re.compile(
    r"(?i)(private[_ -]?key|secret|password|plaintext|input_bytes|result_bytes)"
)
_FORBIDDEN_LIFECYCLE_FIELD_RE = re.compile(
    r"(?i)(payload|content|token|bytes|plaintext|credential|private[_ -]?key|secret|password)"
)

# Lifecycle records are evidence indexes, not a second data plane.  Keep the
# schema closed per milestone so a future caller cannot smuggle request,
# response, credential, or model bytes into the JSONL trace under a new field.
_LIFECYCLE_FIELD_ALLOWLIST = {
    "INPUT_REFERENCE_PUBLISHED": {"referenceDigest"},
    "REQUEST_SENT": {"requestDigest"},
    "ACK_CLOSED": {"ackSnapshotDigest", "ackCount"},
    "GRAPH_READY": {"graphDigest", "catalogueDigest"},
    "PLACEMENT_DECISION": {
        "candidateId", "candidateDigest", "candidatePriority", "providerCount",
    },
    "ARTIFACTS_READY": {"artifactDigest", "artifactCount"},
    "PLAN_SEALED": {"planDigest"},
    "SELECTION_COMMITTED": {"selectionDigest", "selectedRoleCount"},
    "PROVIDER_EXECUTION_STARTED": {"roleDigest", "providerCount"},
    "TERMINAL_RESPONSE": {"resultDigest", "requestCount", "status"},
}


class RunnerError(ValueError):
    """Raised when a case cannot satisfy the source-bound runner contract."""


@dataclass(frozen=True)
class CaseProcessSpec:
    """One child process in the explicit case runtime.

    The command is produced from the validated case policy and runtime
    identity map.  It is intentionally a data object so the complete launch
    vector can be tested without creating a MiniNDN network.
    """

    name: str
    node: str
    command: str
    ready_marker: str
    startup_phase: str
    runtime: str = "python"


@dataclass(frozen=True)
class CaseRuntimeBinding:
    """Validated, candidate-bound inputs for one MiniNDN case.

    This object is deliberately separate from placement.  The config names
    the processes and their authorized capabilities; the live ACK snapshot
    still owns request-time Provider selection and the sealed role map.
    """

    case: str
    service_name: str
    topology: Path
    policy: Path
    output: Path
    nodes: Mapping[str, Any]
    identities: Mapping[str, Any]
    provider_identities: tuple[str, ...]

    @classmethod
    def from_inputs(cls, case: str, output: Path,
                    inputs: Mapping[str, Any]) -> "CaseRuntimeBinding":
        descriptor = inputs.get("descriptor")
        if not isinstance(descriptor, Mapping):
            raise RunnerError("CASE_RUNTIME_DESCRIPTOR_MISSING")
        if descriptor.get("schema") != "spec180-yolo-case-input-v1":
            raise RunnerError("CASE_RUNTIME_DESCRIPTOR_SCHEMA_UNSUPPORTED")
        if descriptor.get("case") != case:
            raise RunnerError("CASE_RUNTIME_CASE_MISMATCH")
        case_runtime = descriptor.get("caseRuntime")
        if not isinstance(case_runtime, Mapping):
            raise RunnerError("CASE_RUNTIME_MISSING")
        nodes = case_runtime.get("nodes")
        identities = case_runtime.get("identities")
        provider_identities = case_runtime.get("providerIdentities")
        if not isinstance(nodes, Mapping) or not isinstance(identities, Mapping):
            raise RunnerError("CASE_RUNTIME_BINDING_INCOMPLETE")
        # Keep the direct adapter seam fail-closed as well as the environment
        # entrypoint.  A caller may construct CaseRuntimeBinding in a test or
        # future launcher without first invoking validate_inputs(); accepting
        # a free-standing catalogue name here would defer a native publisher
        # failure until after MiniNDN startup.
        try:
            catalogue_data_name, catalogue_signer = _validate_catalog_identity({
                "SPEC180_YOLO_CATALOG_DATA_NAME": str(
                    descriptor.get("catalogueDataName", "")),
                "SPEC180_YOLO_CATALOG_SIGNER": str(
                    descriptor.get("catalogueSigner", "")),
            })
        except KeyError as exc:
            raise RunnerError("CASE_RUNTIME_CATALOG_IDENTITY_MISSING") from exc
        if catalogue_signer != str(identities.get("controller", "")):
            raise RunnerError("CASE_RUNTIME_CATALOG_SIGNER_CONTROLLER_MISMATCH")
        if (not isinstance(provider_identities, list)
                or not provider_identities
                or any(not isinstance(item, str) for item in provider_identities)
                or len(provider_identities) != len(set(provider_identities))
                or any(not _NAME_RE.fullmatch(item) for item in provider_identities)):
            raise RunnerError("CASE_RUNTIME_PROVIDER_IDENTITIES_INVALID")
        provider_ids = tuple(provider_identities)
        node_providers = nodes.get("providers")
        identity_providers = identities.get("providers")
        if not isinstance(node_providers, Mapping) or not isinstance(identity_providers, Mapping):
            raise RunnerError("CASE_RUNTIME_PROVIDER_BINDING_INCOMPLETE")
        if set(node_providers) != set(provider_ids):
            raise RunnerError("CASE_RUNTIME_PROVIDER_NODE_IDENTITY_MISMATCH")
        if set(identity_providers) != set(provider_ids):
            raise RunnerError("CASE_RUNTIME_PROVIDER_IDENTITY_MISMATCH")
        for identity in provider_ids:
            if identity_providers.get(identity) != identity:
                raise RunnerError("CASE_RUNTIME_PROVIDER_IDENTITY_VALUE_MISMATCH")
        if any(not str(nodes.get(key, "")) for key in ("controller", "user", "repo")):
            raise RunnerError("CASE_RUNTIME_NODE_BINDING_INCOMPLETE")
        if any(not str(node_providers.get(identity, "")) for identity in provider_ids):
            raise RunnerError("CASE_RUNTIME_PROVIDER_NODE_BINDING_INCOMPLETE")
        for key in ("controller", "user", "repo", "group", "providerPrefix"):
            if not _NAME_RE.fullmatch(str(identities.get(key, ""))):
                raise RunnerError("CASE_RUNTIME_IDENTITY_INVALID:" + key)
        if not _NAME_RE.fullmatch(str(identities.get("repoServicePrefix", ""))):
            raise RunnerError("CASE_RUNTIME_IDENTITY_INVALID:repoServicePrefix")
        policy = inputs.get("case_policy")
        topology = inputs.get("topology")
        if not isinstance(policy, Path) or not policy.is_file():
            raise RunnerError("CASE_RUNTIME_POLICY_MISSING")
        if not isinstance(topology, Path) or not topology.is_file():
            raise RunnerError("CASE_RUNTIME_TOPOLOGY_MISSING")
        try:
            topology_text = topology.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise RunnerError("CASE_RUNTIME_TOPOLOGY_READ_FAILED") from exc
        declared_nodes = {
            str(nodes[key]) for key in ("controller", "user", "repo")
        }
        declared_nodes.update(str(node_providers[item]) for item in provider_ids)
        if any(not node or re.search(
                rf"(?m)^\s*{re.escape(node)}\s*:", topology_text) is None
               for node in declared_nodes):
            raise RunnerError("CASE_RUNTIME_NODE_NOT_IN_TOPOLOGY")
        expected_policy_digest = str(descriptor.get("casePolicySha256", ""))
        if expected_policy_digest != digest_file(policy):
            raise RunnerError("CASE_RUNTIME_POLICY_DIGEST_MISMATCH")
        # Revalidate the exact isolated policy that the future child processes
        # will consume.  Checking only the original source config would leave
        # a caller able to replace case-policy.json between materialization and
        # network startup with a loader-incompatible document.
        _validate_policy_loader_compatibility(
            _load_document(policy, "case-policy"))
        service_name = str(case_runtime.get("serviceName", ""))
        if not _NAME_RE.fullmatch(service_name):
            raise RunnerError("CASE_RUNTIME_SERVICE_NAME_INVALID")
        return cls(
            case=case,
            service_name=service_name,
            topology=topology,
            policy=policy,
            output=output,
            nodes=nodes,
            identities=identities,
            provider_identities=provider_ids,
        )


class MiniNdnCaseRuntime:
    """Explicit adapter around the maintained MiniNDN YOLO harness.

    Importing this class has no MiniNDN side effect.  The network is created
    only by ``start_network`` after ``CaseRuntimeBinding.from_inputs`` has
    validated the candidate-bound topology, policy, and identity maps.
    """

    def __init__(self, binding: CaseRuntimeBinding,
                 inputs: Mapping[str, Any] | None = None) -> None:
        self.binding = binding
        self.inputs = dict(inputs or {})
        self._legacy = None
        self._ndn = None
        self._started_phases: set[str] = set()
        self._ready_phases: set[str] = set()
        self._catalogue_publication_digest: str | None = None
        self._processes: list[tuple[object, object, Path]] = []
        self._cleanup_complete = False
        self._cleanup_started = False
        self._cleanup_attempts = 0
        self._network_resources = []

    def _record_network_resources(self, label):
        resources = network_resources.capture(self._ndn)
        name = 'network-resources-' + label + '.json'
        self._network_resources.append((name, resources))
        network_resources.write_exclusive(self.binding.output/name, resources)

    def _legacy_module(self):
        if self._legacy is None:
            import importlib
            sys.path.insert(0, str(ROOT / "Experiments"))
            self._legacy = importlib.import_module("NDNSF_DI_Yolo2x2_Minindn")
        return self._legacy

    def start_network(self):
        """Reuse the established NFD/SVS startup path with explicit inputs."""
        if self._cleanup_started:
            raise RunnerError("CASE_RUNTIME_ALREADY_STOPPED")
        if self._ndn is not None:
            raise RunnerError("CASE_RUNTIME_NETWORK_ALREADY_STARTED")
        legacy = self._legacy_module()
        legacy.Minindn.verifyDependencies()
        # MiniNDN's constructor parses the process-wide argv for its own
        # --work-dir/--result-dir options.  The Spec180 launcher has already
        # consumed ``--case`` but must not leak it into that parser; doing so
        # aborts the live run with MiniNDN's usage text before NFD starts.
        saved_argv = sys.argv[:]
        try:
            sys.argv = [saved_argv[0]]
            # The legacy helper defaults to the shared /tmp/minindn tree.
            # That path is unsafe for a case-scoped run: a previous root run
            # can leave an unreadable client.conf, and concurrent cases can
            # otherwise share NFD/SVS state.  Keep all MiniNDN runtime state
            # under this fresh evidence root instead.
            work_dir = self.binding.output / "minindn-work"
            ndn = legacy.Minindn(
                topoFile=str(self.binding.topology),
                workDir=str(work_dir),
            )
        finally:
            sys.argv = saved_argv
        # Retain even a partially started network until its stop succeeds.
        self._ndn = ndn
        try:
            self._record_network_resources('created')
            ndn.start()
            self._record_network_resources('started')
            nfd_app = Spec180SifNfd if sif_runtime_enabled() else legacy.Nfd
            legacy.AppManager(ndn, ndn.net.hosts, nfd_app, logLevel="INFO")
            legacy.perf.wait_for_nfd_sockets(ndn, self.binding.output)
        except Exception as exc:
            cleanup_errors = []
            try:
                ndn.stop()
                self._ndn = None
            except Exception as cleanup_exc:
                cleanup_errors.append("network:" + str(cleanup_exc))
            detail = type(exc).__name__ + ":" + str(exc)
            if cleanup_errors:
                detail += ";cleanup=" + ";".join(cleanup_errors)
            raise RunnerError(
                "CASE_RUNTIME_NETWORK_START_FAILED:" + detail) from exc
        return ndn

    def route_origins(self) -> Mapping[str, tuple[str, ...]]:
        """Return explicit node-to-origin bindings before touching MiniNDN."""
        nodes = self.binding.nodes
        identities = self.binding.identities
        controller_identity = str(identities["controller"])
        user_identity = str(identities["user"])
        repo_identity = str(identities["repo"])
        group_prefix = str(identities["group"])
        repo_prefix = str(identities.get("repoServicePrefix", 
                                        "/NDNSF/DistributedRepo/Object"))
        result: dict[str, tuple[str, ...]] = {}

        def add(node_name: str, prefixes: tuple[str, ...]) -> None:
            result[node_name] = tuple(dict.fromkeys(
                (*result.get(node_name, ()), *prefixes)))

        add(str(nodes["controller"]), (
            controller_identity, controller_identity + "/DKEY",
            controller_identity + "/KEY", group_prefix,
        ))
        add(str(nodes["user"]), (user_identity, group_prefix))
        add(str(nodes["repo"]), (repo_identity, repo_identity + "/KEY",
                                  group_prefix, repo_prefix))
        for identity in self.binding.provider_identities:
            node_name = str(nodes["providers"][identity])
            add(node_name, (identity, identity + "/KEY", group_prefix))
        return result

    def configure_routing(self, ndn=None):
        """Install routes from the case node/identity map, never globals."""
        ndn = ndn or self._ndn
        if ndn is None:
            raise RunnerError("CASE_RUNTIME_NETWORK_NOT_STARTED")
        legacy = self._legacy_module()
        nodes = self.binding.nodes
        identities = self.binding.identities
        controller_node = str(nodes["controller"])
        user_node = str(nodes["user"])
        repo_node = str(nodes["repo"])
        controller_identity = str(identities["controller"])
        user_identity = str(identities["user"])
        repo_identity = str(identities["repo"])
        group_prefix = str(identities["group"])
        repo_prefix = str(identities.get("repoServicePrefix",
                                         "/NDNSF/DistributedRepo/Object"))
        rh = legacy.NdnRoutingHelper(ndn.net, "udp", "link-state")
        for node_name, prefixes in self.route_origins().items():
            rh.addOrigin([ndn.net[node_name]], list(prefixes))
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            legacy.Nfdc.setStrategy(node, controller_identity.rsplit("/", 1)[0],
                                    legacy.Nfdc.STRATEGY_BEST_ROUTE)
            legacy.Nfdc.setStrategy(node, group_prefix,
                                    legacy.Nfdc.STRATEGY_MULTICAST)
            legacy.Nfdc.setStrategy(node, repo_prefix,
                                    legacy.Nfdc.STRATEGY_MULTICAST)

    def initialize_keychains(self, ndn=None, *, dual_signing_certs: bool = True) -> None:
        ndn = ndn or self._ndn
        if ndn is None:
            raise RunnerError("CASE_RUNTIME_NETWORK_NOT_STARTED")
        legacy = self._legacy_module()
        identities = self.binding.identities
        legacy.initialize_di_keychains(
            ndn,
            self.binding.output,
            list(self.binding.provider_identities),
            dual_signing_certs=dual_signing_certs,
            controller_node=str(self.binding.nodes["controller"]),
            app_root=str(identities_parent(self.binding.identities["controller"])),
            controller_identity=str(self.binding.identities["controller"]),
            user_identity=str(self.binding.identities["user"]),
            provider_prefix=str(self.binding.identities.get("providerPrefix", "")),
            repo_identity=str(self.binding.identities["repo"]),
        )

    def process_specs(self, phase: str | None = None) -> tuple[CaseProcessSpec, ...]:
        """Build the candidate-bound Controller/Repo/Provider/User commands.

        This function has no process or filesystem side effects.  It is the
        only place where the Spec180 MiniNDN adapter turns the validated case
        policy into child commands.  In particular, it never calls the legacy
        ``provider_role_assignments`` helper: Provider roles are capabilities
        advertised at startup, while request-time ownership still comes from
        the authenticated ACK snapshot and sealed plan.
        """
        if phase is not None and phase not in STARTUP_PHASES:
            raise RunnerError("CASE_RUNTIME_START_PHASE_INVALID:" + str(phase))
        required = ("package", "registry", "offer_trust_root",
                    "offer_public_key_map", "offer_private_key_map",
                    "envelope_key_file")
        missing = [key for key in required if not self.inputs.get(key)]
        if missing:
            raise RunnerError(
                "CASE_PROCESS_INPUTS_INCOMPLETE:" + ",".join(missing))
        package = Path(self.inputs["package"])
        registry = Path(self.inputs["registry"])
        trust_root = Path(self.inputs["offer_trust_root"])
        public_key_map = Path(self.inputs["offer_public_key_map"])
        private_key_map = Path(self.inputs["offer_private_key_map"])
        envelope_key_file = Path(self.inputs["envelope_key_file"])
        for label, path in (("package", package), ("registry", registry),
                            ("offer-trust-root", trust_root),
                            ("offer-public-key-map", public_key_map),
                            ("offer-private-key-map", private_key_map),
                            ("request-envelope-key", envelope_key_file)):
            valid_type = path.is_dir() if label == "package" else path.is_file()
            if not path.is_absolute() or not valid_type or not os.access(
                    path, os.R_OK | (os.X_OK if label == "package" else 0)):
                raise RunnerError("CASE_PROCESS_INPUT_INVALID:" + label)

        descriptor = self.inputs.get("descriptor")
        if not isinstance(descriptor, Mapping):
            raise RunnerError("CASE_PROCESS_DESCRIPTOR_MISSING")

        def verify_declared_digest(field: str, path: Path, code: str) -> None:
            expected = descriptor.get(field)
            if expected is None:
                return
            if (not _DIGEST_RE.fullmatch(str(expected))
                    or str(expected) != digest_file(path)):
                raise RunnerError(code)

        # Recheck every candidate-bound file after the process vector is built.
        # validate_inputs() created the descriptor earlier, but a caller could
        # otherwise replace a map, policy, or package between preflight and
        # this last zero-side-effect boundary.
        verify_declared_digest(
            "packageManifestSha256", package / "manifest.json",
            "CASE_PROCESS_PACKAGE_DIGEST_MISMATCH")
        verify_declared_digest(
            "catalogueRegistrySha256", registry,
            "CASE_PROCESS_REGISTRY_DIGEST_MISMATCH")
        verify_declared_digest(
            "offerTrustRootSha256", trust_root,
            "CASE_PROCESS_OFFER_TRUST_ROOT_DIGEST_MISMATCH")
        verify_declared_digest(
            "offerPublicKeyMapSha256", public_key_map,
            "CASE_PROCESS_OFFER_PUBLIC_KEY_MAP_DIGEST_MISMATCH")
        verify_declared_digest(
            "offerPrivateKeyMapSha256", private_key_map,
            "CASE_PROCESS_OFFER_PRIVATE_KEY_MAP_DIGEST_MISMATCH")
        verify_declared_digest(
            "requestEnvelopeKeySha256", envelope_key_file,
            "CASE_PROCESS_REQUEST_ENVELOPE_KEY_DIGEST_MISMATCH")
        verify_declared_digest(
            "topologySha256", self.binding.topology,
            "CASE_PROCESS_TOPOLOGY_DIGEST_MISMATCH")
        config_input = self.inputs.get("config")
        if config_input is not None:
            config_path = Path(config_input)
            if (not config_path.is_absolute() or not config_path.is_file()
                    or not os.access(config_path, os.R_OK)):
                raise RunnerError("CASE_PROCESS_INPUT_INVALID:config")
            verify_declared_digest(
                "configSha256", config_path,
                "CASE_PROCESS_CONFIG_DIGEST_MISMATCH")
        catalogue_data_name = str(descriptor.get("catalogueDataName", ""))
        catalogue_signer = str(descriptor.get("catalogueSigner", ""))
        if not (_NAME_RE.fullmatch(catalogue_data_name)
                and _NAME_RE.fullmatch(catalogue_signer)):
            raise RunnerError("CASE_PROCESS_CATALOG_IDENTITY_INVALID")

        policy = self.binding.policy
        generated = self.binding.output / "generated-policy"
        common = ["--config", str(policy), "--generated-policy-dir", str(generated)]
        legacy = self._legacy_module()
        py_dir = Path(legacy.PY_DIR)
        repo = Path(legacy.REPO)
        commands: list[CaseProcessSpec] = []

        def python_command(script: str, argv: list[str]) -> str:
            if not sif_runtime_enabled():
                return legacy.python_cmd(script, argv, repo=repo, py_dir=py_dir)
            # Inside the exact-SIF replay the application lives under the
            # sealed repo copy; the Apptainer command provider supplies the
            # interpreter and the working directory.  Never reference the
            # host checkout paths from a child command.
            sif_py_dir = Path(SIF_RUNTIME_REPO) / "examples/python" / (
                "NDNSF-DistributedInference/yolo_2x2")
            args = " ".join(
                [f"{sif_py_dir / script}"] + [f'"{arg}"' for arg in argv])
            return args

        def native_provider_command(*, identity: str, roles: tuple[str, ...],
                                    key_path: Path) -> str:
            generated_plan = generated / "native-execution-plan.json"
            generated_manifest = generated / "service-manifest.json"
            generated_trust_schema = generated / "trust-schema.conf"
            if sif_runtime_enabled():
                executable = SIF_RUNTIME_BIN + "/di-native-provider"
            else:
                executable = os.environ.get(
                    "SPEC180_NATIVE_PROVIDER_BINARY",
                    str(ROOT / "build-system-j2/examples/di-native-provider"),
                )
            argv = [
                executable,
                "--serve",
                "--plan", str(generated_plan),
                "--manifest", str(generated_manifest),
                "--service", self.binding.service_name,
                "--provider", identity,
                "--group", str(identities["group"]),
                "--controller", str(identities["controller"]),
                "--trust-schema", str(generated_trust_schema),
                "--roles", ",".join(roles),
                "--workers", "1",
                "--handler-threads", "1",
                "--ack-threads", "1",
                "--artifact-cache-dir", str(
                    self.binding.output / "native-artifact-cache" /
                    identity.rsplit("/", 1)[-1]),
                "--selection-offer-key-file", str(key_path),
                "--offer-backend", "onnxruntime-cpu",
                "--offer-can-provision",
                "--permission-wait-ms", "60000",
            ]
            quoted = " ".join(shlex.quote(item) for item in argv)
            if sif_runtime_enabled():
                return quoted
            return ("cd " + shlex.quote(str(ROOT)) +
                    " && exec " + quoted)

        identities = self.binding.identities
        nodes = self.binding.nodes
        publication_file = self.inputs.get("runtime_publication_file")
        if publication_file:
            publication_file = Path(publication_file)
            if (not publication_file.is_absolute() or
                    not publication_file.is_file() or
                    not os.access(publication_file, os.R_OK)):
                raise RunnerError("CASE_PROCESS_RUNTIME_PUBLICATION_INVALID")
        controller_args = list(common)
        if publication_file is not None:
            controller_args += [
                "--spec180-runtime-publication-file", str(publication_file),
            ]
        controller_ready_marker = (
            "SPEC180_RUNTIME_CATALOGUE_PUBLISHED"
            if publication_file is not None else "SPEC180_CONTROLLER_READY")
        commands.append(CaseProcessSpec(
            "controller", str(nodes["controller"]),
            python_command("controller.py", controller_args),
            controller_ready_marker,
            "control",
        ))

        # The repository identity is a declared runtime input.  A repo node is
        # represented as a Provider identity by RepoNodeApp, so derive its
        # suffix only from the explicit provider namespace; never guess from
        # the old AI_LAB/D role list.
        provider_prefix = str(identities.get("providerPrefix", "")).rstrip("/")
        repo_identity = str(identities.get("repo", ""))
        repo_marker = provider_prefix + "/"
        if not repo_identity.startswith(repo_marker):
            raise RunnerError("CASE_PROCESS_REPO_IDENTITY_OUTSIDE_PROVIDER_PREFIX")
        repo_id = repo_identity[len(repo_marker):]
        if not repo_id or "/" in repo_id:
            raise RunnerError("CASE_PROCESS_REPO_PROVIDER_ID_INVALID")
        commands.append(CaseProcessSpec(
            "repo", str(nodes["repo"]),
            python_command("repo_node.py", common + [
                "--provider-id", repo_id,
                "--repo-node", repo_identity,
                "--failure-domain", "spec180-repo",
                "--storage-dir", str(self.binding.output / "repo-store"),
                "--handler-threads", "1", "--ack-threads", "1",
            ]),
            "Installed provider permission", "control",
        ))

        service = self.binding.service_name
        policy_doc = _load_document(policy, "case-policy")
        services = policy_doc.get("services")
        service_doc = next((item for item in services or ()
                            if isinstance(item, Mapping)
                            and str(item.get("name", "")) == service), None)
        if not isinstance(service_doc, Mapping):
            raise RunnerError("CASE_PROCESS_SERVICE_NOT_FOUND")
        provider_docs = {
            str(item.get("identity", "")): item
            for item in service_doc.get("providers", ())
            if isinstance(item, Mapping)
        }
        private_key_entries = _load_document(
            private_key_map, "offer-private-key-map")
        if not isinstance(private_key_entries, Mapping):
            raise RunnerError("CASE_PROCESS_OFFER_PRIVATE_KEY_MAP_INVALID")
        declared_private_key_digests = descriptor.get(
            "offerPrivateKeysSha256")
        if (declared_private_key_digests is not None
                and not isinstance(declared_private_key_digests, Mapping)):
            raise RunnerError("CASE_PROCESS_OFFER_PRIVATE_KEY_DIGESTS_INVALID")
        for identity in self.binding.provider_identities:
            provider_doc = provider_docs.get(identity)
            if not isinstance(provider_doc, Mapping):
                raise RunnerError("CASE_PROCESS_PROVIDER_NOT_AUTHORIZED:" + identity)
            roles = tuple(str(role) for role in provider_doc.get("roles", ())
                          if str(role))
            if not roles:
                raise RunnerError("CASE_PROCESS_PROVIDER_ROLES_EMPTY:" + identity)
            # Y-N-C is a live capability mutation.  Keep the signed policy
            # and catalogue inputs unchanged, but start the real native
            # Provider with the advertised capability removed.  This is the
            # process-boundary witness for the fixed no-feasible-candidate
            # case; it must not be satisfied by an offline ACK fixture.
            if str(self.inputs.get("subcase", "")) == "Y-N-C":
                roles = tuple(role for role in roles
                              if role not in {"FullModel", "Merge"})
                if not roles:
                    # The command-line parser rejects an empty capability
                    # list.  A harmless remaining role keeps the Provider
                    # alive while its ACK capability set is still missing
                    # the required shared Merge role.
                    roles = ("BackboneNeck",)
            if not identity.startswith(repo_marker):
                raise RunnerError("CASE_PROCESS_PROVIDER_IDENTITY_INVALID:" + identity)
            provider_id = identity[len(repo_marker):]
            if not provider_id or "/" in provider_id:
                raise RunnerError("CASE_PROCESS_PROVIDER_ID_INVALID:" + identity)
            key_file = private_key_entries.get(identity)
            if (not isinstance(key_file, str) or not key_file
                    or not Path(key_file).expanduser().is_absolute()):
                raise RunnerError(
                    "CASE_PROCESS_OFFER_PRIVATE_KEY_MISSING:" + identity)
            key_path = Path(key_file).expanduser()
            if (not key_path.is_file() or not os.access(key_path, os.R_OK)):
                raise RunnerError(
                    "CASE_PROCESS_OFFER_PRIVATE_KEY_INVALID:" + identity)
            expected_key_digest = (
                declared_private_key_digests.get(identity)
                if declared_private_key_digests is not None else None)
            if (expected_key_digest is not None
                    and (not _DIGEST_RE.fullmatch(str(expected_key_digest))
                         or str(expected_key_digest) != digest_file(key_path))):
                raise RunnerError(
                    "CASE_PROCESS_OFFER_PRIVATE_KEY_DIGEST_MISMATCH:" + identity)
            node = str(nodes["providers"][identity])
            # Both epochs exercise the native production owner. Protected
            # assignments must pass its grant factory before preparation.
            commands.append(CaseProcessSpec(
                "provider-" + provider_id, node,
                native_provider_command(
                    identity=identity, roles=roles, key_path=key_path),
                "NDNSF_DI_NATIVE_PROVIDER_READY", "providers", "native",
            ))

        user_args = common + [
            "--canonical-package", str(package),
            "--catalogue-registry", str(registry),
            "--offer-trust-root", str(trust_root),
            "--offer-public-key-map", str(public_key_map),
            "--catalog-data-name", catalogue_data_name,
            "--catalog-signer", catalogue_signer,
            "--ack-timeout-ms", "1500",
            "--timeout-ms", "60000",
            # The canonical YOLO26n graph is pinned at 640-by-640 static input
            # (T004); the legacy 32-by-32 default cannot feed it.
            "--input-size", "640",
            "--sequential-requests", "1",
            "--request-id",
            "/spec180-" + str(self.inputs.get("lifecycle_case",
                                                self.binding.case)).lower() + "-" + hashlib.sha256(
                str(self.binding.output).encode("utf-8")).hexdigest()[:16],
            "--lifecycle-output-dir", str(self.binding.output),
            "--lifecycle-case", str(self.inputs.get("lifecycle_case",
                                                     self.binding.case)),
            "--envelope-key-file", str(envelope_key_file),
            "--native-tensor-input",
        ]
        commands.append(CaseProcessSpec(
            "user", str(nodes["user"]), python_command("user.py", user_args),
            "YOLO_ACK_DRIVEN_RESULT", "user",
        ))
        if phase is None:
            return tuple(commands)
        return tuple(item for item in commands if item.startup_phase == phase)

    def _sif_role_home(self, process_name: str) -> str:
        """Return the provisioned PIB/TPM home for one exact-SIF child.

        MiniNDN assigns each node a fresh ``minindn-work/<node>`` HOME for
        NFD.  Application children need the issuer-signed role material from
        the provision stage instead; using the node HOME silently creates a
        second self-signed trust domain.  Keep NFD on its node HOME and bind
        only the application child to this immutable role HOME.
        """
        if process_name in {"controller", "repo", "user"}:
            role = process_name
        elif process_name.startswith("provider-"):
            role = process_name[len("provider-"):]
        else:
            raise RunnerError("SIF_ROLE_NAME_INVALID:" + process_name)
        if (not role or "/" in role or ".." in role
                or not re.fullmatch(r"[A-Za-z0-9_.-]+", role)):
            raise RunnerError("SIF_ROLE_NAME_INVALID:" + process_name)
        # A Y-N matrix owns one nested output directory per subcase, while
        # Y-B keeps its evidence directly under host-minindn/output.  Resolve
        # the run-scoped private tree by walking ancestors instead of assuming
        # one fixed depth; otherwise nested SIF cases look for
        # ``host-minindn/private`` and fail before the first child starts.
        private_root = None
        for ancestor in (self.binding.output, *self.binding.output.parents):
            candidate = ancestor / "private"
            if candidate.is_dir() and not candidate.is_symlink():
                private_root = candidate
                break
        if private_root is None:
            raise RunnerError("SIF_PRIVATE_ROOT_MISSING")
        home = private_root / role
        if (home.is_symlink() or not home.is_dir()
                or any(parent.is_symlink() for parent in (home, *home.parents))
                or not (home / ".ndn/pib.db").is_file()
                or not (home / ".ndn/ndnsec-key-file").is_dir()):
            raise RunnerError("SIF_ROLE_HOME_INVALID:" + process_name)
        return str(home.resolve())

    def mark_catalogue_published(self, *, data_name: str, signer: str,
                                 data_digest: str) -> None:
        """Record a validated catalogue publication before starting User.

        The live driver calls this only after ``ServiceUser`` has successfully
        published and read back the exact signed APP Data record.  The method
        records the receipt boundary; it does not publish data or choose a
        candidate itself.
        """
        if "providers" not in self._ready_phases:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_PHASE_INVALID")
        if self._catalogue_publication_digest is not None:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_DUPLICATE")
        descriptor = self.inputs.get("descriptor")
        expected_name = str(descriptor.get("catalogueDataName", "")) \
            if isinstance(descriptor, Mapping) else ""
        expected_signer = str(descriptor.get("catalogueSigner", "")) \
            if isinstance(descriptor, Mapping) else ""
        if data_name != expected_name or not _NAME_RE.fullmatch(data_name):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_NAME_MISMATCH")
        if signer != expected_signer or not _NAME_RE.fullmatch(signer):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_SIGNER_MISMATCH")
        if not _DIGEST_RE.fullmatch(str(data_digest)):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_DIGEST_INVALID")
        self._catalogue_publication_digest = str(data_digest)

    def runtime_catalogue_payload(
            self,
            snapshots: tuple[Any, ...] | list[Any],
    ) -> bytes:
        """Build the exact active snapshot payload for this case.

        This method is deliberately separate from ``mark_catalogue_published``:
        construction validates candidate/artifact coverage, while the live
        driver still must sign, publish, and verify the returned bytes through
        the controller-owned APP API before starting User.
        """
        descriptor = self.inputs.get("descriptor")
        if not isinstance(descriptor, Mapping):
            raise RunnerError("CASE_RUNTIME_DESCRIPTOR_MISSING")
        manifest = self.inputs.get("manifest")
        if not isinstance(manifest, Mapping):
            raise RunnerError("CASE_RUNTIME_MANIFEST_MISSING")
        return build_runtime_catalogue_payload(
            self.binding.case,
            str(descriptor.get("catalogueDataName", "")),
            manifest,
            snapshots,
        )

    def publish_and_verify_runtime_catalogue(
            self,
            publisher: Any,
            snapshots: tuple[Any, ...] | list[Any],
            *,
            freshness_ms: int = 60000,
            fetch_timeout_ms: int = 5000,
    ) -> Mapping[str, str]:
        """Publish and read back the candidate-bound runtime APP catalogue.

        The runner owns the payload composition and publication barrier, while
        ``ServiceUser`` remains the signing and NDN transport authority.  A
        successful publish call alone is insufficient: the exact name and
        payload must be fetched through the expected-signer path before the
        receipt can unlock the User phase.  This method is intentionally
        publisher-injected so the protocol boundary can be tested without
        creating MiniNDN; the production driver supplies the controller-node
        ``ServiceUser`` instance.
        """
        if "providers" not in self._ready_phases:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_PHASE_INVALID")
        if self._catalogue_publication_digest is not None:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_DUPLICATE")
        if int(freshness_ms) <= 0 or int(fetch_timeout_ms) <= 0:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_TIMEOUT_INVALID")
        publish = getattr(publisher, "publish_signed_app_data", None)
        fetch = getattr(publisher, "fetch_signed_app_data", None)
        if not callable(publish) or not callable(fetch):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLISHER_INVALID")
        descriptor = self.inputs.get("descriptor")
        if not isinstance(descriptor, Mapping):
            raise RunnerError("CASE_RUNTIME_DESCRIPTOR_MISSING")
        data_name = str(descriptor.get("catalogueDataName", ""))
        signer = str(descriptor.get("catalogueSigner", ""))
        if not (_NAME_RE.fullmatch(data_name) and _NAME_RE.fullmatch(signer)):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_IDENTITY_INVALID")
        payload = self.runtime_catalogue_payload(snapshots)
        try:
            published = publish(data_name, payload,
                               freshness_ms=int(freshness_ms))
        except Exception as exc:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLISH_FAILED") from exc
        if (not bool(getattr(published, "success", False))
                or str(getattr(published, "data_name", "")) != data_name):
            reason = str(getattr(published, "error", "publish failed"))
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLISH_FAILED:" + reason)
        try:
            received = fetch(data_name, signer,
                             timeout_ms=int(fetch_timeout_ms))
        except Exception as exc:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_READBACK_FAILED") from exc
        if not bool(getattr(received, "success", False)):
            reason = str(getattr(received, "error", "readback failed"))
            raise RunnerError("CASE_RUNTIME_CATALOGUE_READBACK_FAILED:" + reason)
        if str(getattr(received, "data_name", "")) != data_name:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_READBACK_NAME_MISMATCH")
        if bytes(getattr(received, "payload", b"")) != bytes(payload):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_READBACK_DIGEST_MISMATCH")
        certificate = str(getattr(received, "signer_certificate", ""))
        if not certificate or not (certificate == signer or
                                   certificate.startswith(signer + "/")):
            raise RunnerError("CASE_RUNTIME_CATALOGUE_READBACK_SIGNER_MISMATCH")
        digest = digest_bytes(payload)
        self.mark_catalogue_published(
            data_name=data_name, signer=signer, data_digest=digest)
        return {
            "dataName": data_name,
            "signer": signer,
            "payloadDigest": digest,
        }

    def start_processes(self, ndn, env: Mapping[str, str],
                        procs: list[tuple[object, object, Path]], *,
                        phase: str):
        """Start one explicit process phase in the required barrier order.

        The live driver must call this method as ``control`` (then wait for
        readiness), ``providers`` (then wait again), and finally ``user``
        after catalogue publication.  A single all-process launch would allow
        the User to issue ``REQUEST_SENT`` before the signed catalogue and
        provider services are ready, so no implicit "start everything" mode is
        provided.
        """
        if ndn is None:
            raise RunnerError("CASE_RUNTIME_NETWORK_NOT_STARTED")
        if phase not in STARTUP_PHASES:
            raise RunnerError("CASE_RUNTIME_START_PHASE_INVALID:" + str(phase))
        if phase in self._started_phases:
            raise RunnerError("CASE_RUNTIME_START_PHASE_DUPLICATE:" + phase)
        phase_index = STARTUP_PHASES.index(phase)
        missing_phases = [item for item in STARTUP_PHASES[:phase_index]
                          if item not in self._started_phases]
        if missing_phases:
            raise RunnerError(
                "CASE_RUNTIME_START_PHASE_PRECONDITION:" + ",".join(
                    missing_phases))
        missing_ready = [item for item in STARTUP_PHASES[:phase_index]
                         if item not in self._ready_phases]
        if missing_ready:
            raise RunnerError(
                "CASE_RUNTIME_READY_PHASE_PRECONDITION:" + ",".join(
                    missing_ready))
        if phase == "user" and self._catalogue_publication_digest is None:
            raise RunnerError("CASE_RUNTIME_CATALOGUE_PUBLICATION_REQUIRED")
        specs = self.process_specs(phase)
        if not specs:
            raise RunnerError("CASE_RUNTIME_START_PHASE_EMPTY:" + phase)
        network = getattr(ndn, "net", None)
        if network is None:
            raise RunnerError("CASE_RUNTIME_NETWORK_MISSING")
        # Resolve every node before creating the first child.  This keeps a
        # malformed node map from leaving an earlier process running.
        node_handles = {}
        for spec in specs:
            try:
                node_handles[spec.node] = network[spec.node]
            except (KeyError, IndexError, TypeError) as exc:
                raise RunnerError(
                    "CASE_RUNTIME_NODE_NOT_IN_NETWORK:" + spec.node) from exc
        legacy = self._legacy_module()
        started = []
        phase_start = len(procs)
        try:
            for spec in specs:
                # MiniNDN runs one NFD per node.  Bind each child explicitly to
                # its node socket instead of relying on whichever client.conf
                # a native ndn-cxx build happens to discover through HOME.
                node_env = dict(env)
                # Protected Y-B User consumes the candidate-bound public
                # recipient map.  Providers consume their private recipient
                # map; exposing both through one shared environment makes the
                # User's fail-closed map selector reject an otherwise valid
                # startup.  Keep the private map for Provider children only.
                if (spec.name == "user"
                        and self.inputs.get("protection_epoch", PLAINTEXT_EPOCH)
                        != PLAINTEXT_EPOCH):
                    node_env.pop("SPEC181_PROVIDER_RECIPIENT_KEY_MAP", None)
                node_env["NDN_CLIENT_TRANSPORT"] = (
                    "unix:///run/nfd/" + str(spec.node) + ".sock")
                sif_home = (self._sif_role_home(spec.name)
                            if sif_runtime_enabled() else None)
                node_transport = node_env["NDN_CLIENT_TRANSPORT"]
                # The exact-SIF replay contract prefixes every application
                # child with the Apptainer command provider; the host process
                # fallback marker guards the replay driver's contract check.
                if spec.runtime == "native" and sif_runtime_enabled():
                    app_prefix = sif_exec_prefix(
                        node_env, home_dir=sif_home) + " "
                else:
                    app_prefix = sif_python_prefix(
                        node_env, home_dir=sif_home)
                node_command = (
                    "export NDN_CLIENT_TRANSPORT='" + node_transport + "'; "
                    + ("exec " + app_prefix if app_prefix else "")
                    + spec.command)
                proc, log_path = legacy.start(
                    node_handles[spec.node], spec.name, node_command,
                    node_env, procs, output_dir=self.binding.output,
                    artifact_cache_root=self.binding.output / "artifact-cache",
                )
                started.append((spec, proc, log_path))
                # The repository's ServiceProvider constructor fetches the
                # controller's public parameters.  Starting it concurrently
                # with the Controller creates a startup race, even though
                # both belong to the documented control phase.  Close the
                # Controller readiness barrier before launching the repo.
                if (phase == "control" and spec.name == "controller"
                        and len(specs) > 1):
                    wait_for_ready(tuple(started), 90.0)
        except Exception as exc:
            # A phase is atomic: if one child cannot be launched, do not leave
            # earlier children running while the caller reports a pre-start
            # failure.  Preserve already-started earlier phases in ``procs``
            # and remove only this phase's entries after bounded cleanup.
            phase_procs = procs[phase_start:]
            if phase_procs:
                try:
                    legacy.stop_process_group(phase_procs)
                except Exception:
                    # Preserve ownership for the outer finally/retry. Failed
                    # cleanup must not erase the only remaining child handles.
                    self._processes.extend(phase_procs)
                    raise
                else:
                    del procs[phase_start:]
            # The outer matrix deliberately reports a bounded failure code.
            # Preserve the underlying location before that wrapping loses it,
            # without serializing exception messages, arguments, or locals.
            frames = []
            frame = exc.__traceback__
            while frame is not None:
                frames.append({
                    "file": frame.tb_frame.f_code.co_filename,
                    "function": frame.tb_frame.f_code.co_name,
                    "line": frame.tb_lineno,
                })
                frame = frame.tb_next
            failure = {
                "schema": "spec180-process-start-failure-v1",
                "phase": phase, "errorType": type(exc).__name__,
                "frames": frames,
            }
            try:
                with (self.binding.output / "process-start-failure.json").open("x") as record:
                    json.dump(failure, record, sort_keys=True, indent=2)
                    record.write("\n")
            except OSError:
                # Retain existing evidence and the original failure even if
                # this optional diagnostic cannot be written.
                pass
            raise RunnerError(
                "CASE_RUNTIME_PROCESS_START_FAILED:" + phase) from exc
        self._started_phases.add(phase)
        # ``legacy.start`` appends the process handles expected by
        # ``legacy.stop_process_group`` to ``procs``.  Keep those handles for
        # teardown; ``started`` additionally carries CaseProcessSpec and is
        # only the readiness view returned to this runner.
        self._processes.extend(procs[phase_start:])
        return tuple(started)

    def stop(self) -> None:
        """Stop only this case's children and MiniNDN network.

        The live driver must call this from ``finally`` after every phase,
        publication, or request attempt.  Cleanup is idempotent and does not
        depend on a successful ACK/Response. This initiates owned teardown;
        tracked child reaping is checked by the shared helper. Descendant and
        network-wide deadline qualification remain separate requirements.
        """
        if self._cleanup_complete:
            return
        legacy = self._legacy_module()
        self._cleanup_started = True
        self._cleanup_attempts += 1
        errors: list[str] = []
        owned = list(self._processes)
        records = []
        if owned:
            try:
                records = legacy.stop_process_group(owned)
                self._processes.clear()
            except Exception as exc:
                records = getattr(exc, 'records', [])
                errors.append("processes:" + str(exc))
        network = self._ndn
        if network is not None:
            try:
                network.stop()
                self._ndn = None
            except Exception as exc:
                errors.append("network:" + str(exc))
        # Minindn.cleanUp() is host-global (including unrelated processes and
        # interfaces). The network instance and tracked child handles above
        # are the only resources this case is authorized to tear down.
        self._started_phases.clear()
        self._ready_phases.clear()
        self._catalogue_publication_digest = None
        observations = []
        if self._network_resources:
            try:
                combined = network_resources.combine([row[1] for row in self._network_resources])
                # ``Minindn.stop()`` asks NFD and its namespace workers to
                # leave asynchronously.  A single immediate /proc scan can
                # therefore observe a process that is already in teardown
                # and turn an otherwise clean case into a false cleanup red.
                # Give owned resources a short, explicit settle window while
                # retaining the fail-closed result for anything that remains
                # after the bound.
                settle_deadline = time.monotonic() + 5.0
                observation = None
                while True:
                    remaining = settle_deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    observation = network_resources.inspect(
                        combined, seconds=max(0.1, remaining))
                    if observation['clean']:
                        break
                    remaining = settle_deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    time.sleep(min(0.2, remaining))
                if observation is None:
                    raise RuntimeError('NETWORK_RESOURCE_SETTLE_TIMEOUT')
                observations.append(dict(resourceSnapshots=[row[0] for row in self._network_resources],
                                         observation=observation))
                if not observation['clean']:
                    errors.append('network-resources:REMAINING')
            except Exception as exc:
                errors.append('network-resources:' + type(exc).__name__)
        record = dict(schema='minindn-owned-cleanup-v1',
                      attempt=self._cleanup_attempts, children=records,
                      networkResourceObservations=observations,
                      networkStopped=self._ndn is None, errors=errors,
                      qualification='NOT_EVALUATED')
        try:
            with (self.binding.output / (
                    'cleanup-attempt-%03d.json' % self._cleanup_attempts)).open('x') as stream:
                json.dump(record, stream, sort_keys=True, indent=2)
                stream.write('\n')
        except OSError as exc:
            errors.append('record:' + type(exc).__name__)
        self._cleanup_complete = not errors
        if errors:
            raise RunnerError("CASE_RUNTIME_CLEANUP_FAILED:" + ";".join(errors))

    def wait_for_ready(
            self, started: tuple[tuple[CaseProcessSpec, object, Path], ...],
            timeout_s: float) -> None:
        """Close the readiness barrier for one already-started phase."""
        if not started:
            raise RunnerError("CASE_RUNTIME_READY_SET_EMPTY")
        phases = {spec.startup_phase for spec, _proc, _path in started}
        if len(phases) != 1:
            raise RunnerError("CASE_RUNTIME_READY_PHASE_MIXED")
        phase = next(iter(phases))
        if phase not in self._started_phases:
            raise RunnerError("CASE_RUNTIME_READY_PHASE_NOT_STARTED:" + phase)
        wait_for_ready(started, timeout_s)
        self._ready_phases.add(phase)


def wait_for_ready(started: tuple[tuple[CaseProcessSpec, object, Path], ...],
                   timeout_s: float) -> None:
    """Wait for every process in one phase to publish its declared marker.

    Readiness is a process-level barrier, not a sleep.  A child that exits
    before its marker or a missing marker at the deadline fails the case and
    leaves the caller responsible for bounded cleanup of already-started
    children.
    """
    if not started:
        raise RunnerError("CASE_RUNTIME_READY_SET_EMPTY")
    if timeout_s <= 0:
        raise RunnerError("CASE_RUNTIME_READY_TIMEOUT_INVALID")
    deadline = time.monotonic() + float(timeout_s)
    pending = list(started)
    while pending:
        remaining: list[tuple[CaseProcessSpec, object, Path]] = []
        for spec, proc, log_path in pending:
            try:
                text = log_path.read_text(errors="replace") if log_path.exists() else ""
            except OSError as exc:
                raise RunnerError(
                    "CASE_RUNTIME_READY_LOG_READ_FAILED:" + spec.name) from exc
            if spec.ready_marker and spec.ready_marker in text:
                continue
            poll = getattr(proc, "poll", None)
            return_code = poll() if callable(poll) else None
            if return_code is not None:
                raise RunnerError(
                    f"CASE_RUNTIME_PROCESS_EXITED_BEFORE_READY:{spec.name}:"
                    f"returncode={return_code}")
            remaining.append((spec, proc, log_path))
        pending = remaining
        if not pending:
            return
        wait_for = deadline - time.monotonic()
        if wait_for <= 0:
            raise RunnerError(
                "CASE_RUNTIME_READY_TIMEOUT:" + ",".join(
                    spec.name for spec, _proc, _path in pending))
        time.sleep(min(0.2, wait_for))


def identities_parent(identity: str) -> str:
    """Return a namespace prefix without inventing a process identity."""
    value = str(identity).rstrip("/")
    if "/" not in value[1:]:
        raise RunnerError("CASE_RUNTIME_IDENTITY_NAMESPACE_INVALID")
    return value.rsplit("/", 1)[0]


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    try:
        return digest_bytes(path.read_bytes())
    except OSError as exc:
        raise RunnerError("FILE_READ_FAILED:" + str(path)) from exc


def _load_document(path: Path, label: str) -> Mapping[str, Any]:
    if not path.is_file():
        raise RunnerError("FILE_MISSING:" + label)
    try:
        raw = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise RunnerError("FILE_READ_FAILED:" + label) from exc
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        try:
            import yaml  # type: ignore
            value = yaml.safe_load(raw)
        except Exception as exc:  # pragma: no cover - optional dependency
            raise RunnerError("DOCUMENT_NOT_JSON_OR_YAML:" + label) from exc
    if not isinstance(value, Mapping):
        raise RunnerError("DOCUMENT_NOT_OBJECT:" + label)
    return value


def _absolute_file(value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        raise RunnerError("PATH_NOT_ABSOLUTE:" + label)
    path = path.resolve()
    if not path.is_file():
        raise RunnerError("FILE_MISSING:" + label)
    if not os.access(path, os.R_OK):
        raise RunnerError("FILE_NOT_READABLE:" + label)
    return path


def _absolute_directory(value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        raise RunnerError("PATH_NOT_ABSOLUTE:" + label)
    path = path.resolve()
    if not path.is_dir():
        raise RunnerError("DIRECTORY_MISSING:" + label)
    if not os.access(path, os.R_OK | os.X_OK):
        raise RunnerError("DIRECTORY_NOT_READABLE:" + label)
    return path


def _validate_output_root(value: str) -> Path:
    if not value:
        raise RunnerError("ENVIRONMENT_MISSING:SPEC180_CASE_OUTPUT_DIR")
    path = Path(value)
    if not path.is_absolute():
        raise RunnerError("OUTPUT_ROOT_NOT_ABSOLUTE")
    path = path.resolve()
    if not path.exists():
        raise RunnerError("OUTPUT_ROOT_MISSING")
    if not path.is_dir():
        raise RunnerError("OUTPUT_ROOT_NOT_DIRECTORY")
    try:
        if any(path.iterdir()):
            raise RunnerError("OUTPUT_ROOT_NOT_EMPTY")
    except OSError as exc:
        raise RunnerError("OUTPUT_ROOT_UNREADABLE") from exc
    return path


def _validate_state_root(value: str) -> Path:
    """Require a persistent, absolute operator journal root before startup."""
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise RunnerError("STATE_ROOT_NOT_ABSOLUTE")
    resolved = path.resolve(strict=False)
    if any(
        resolved == volatile or volatile in resolved.parents
        for volatile in (Path("/tmp"), Path("/run"), Path("/dev/shm"))
    ):
        raise RunnerError("STATE_ROOT_VOLATILE")
    if path.exists() and path.is_symlink():
        raise RunnerError("STATE_ROOT_SYMLINK")
    if path.exists():
        if not path.is_dir():
            raise RunnerError("STATE_ROOT_NOT_DIRECTORY")
        owner_uid = path.stat().st_uid
        # ``runtime.host_minindn`` uses the system manager so it can create
        # network namespaces and NFD sockets.  The transient unit therefore
        # runs as root even when the operator invoked this wrapper as an
        # unprivileged user.  Bind the pre-created state directory to that
        # operator UID before crossing the privilege boundary; never accept
        # an arbitrary owner for a root-launched child.
        allowed_uids = {os.geteuid()}
        if os.geteuid() == 0:
            declared_owner = os.environ.get(STATE_ROOT_OWNER_ENV, "").strip()
            if declared_owner.isdigit():
                allowed_uids.add(int(declared_owner))
        if owner_uid not in allowed_uids:
            raise RunnerError("STATE_ROOT_OWNER_MISMATCH")
    return resolved


def _validate_envelope_key_file(value: str) -> Path:
    """Validate the external owner-managed APP request-envelope key.

    The key remains outside case descriptors and evidence. The runner binds
    only its digest and passes the protected file explicitly to the User.
    """
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise RunnerError("REQUEST_ENVELOPE_KEY_NOT_ABSOLUTE")
    if path.is_symlink():
        raise RunnerError("REQUEST_ENVELOPE_KEY_SYMLINK")
    path = path.resolve(strict=False)
    if not path.is_file() or not os.access(path, os.R_OK):
        raise RunnerError("REQUEST_ENVELOPE_KEY_UNAVAILABLE")
    file_stat = path.stat()
    allowed_uids = {os.geteuid()}
    if os.geteuid() == 0:
        declared_owner = os.environ.get(STATE_ROOT_OWNER_ENV, "").strip()
        if declared_owner.isdigit():
            allowed_uids.add(int(declared_owner))
    if file_stat.st_uid not in allowed_uids:
        raise RunnerError("REQUEST_ENVELOPE_KEY_OWNER_MISMATCH")
    if file_stat.st_mode & 0o077:
        raise RunnerError("REQUEST_ENVELOPE_KEY_PERMISSIONS_INVALID")
    try:
        key_size = len(path.read_bytes())
    except OSError as exc:
        raise RunnerError("REQUEST_ENVELOPE_KEY_UNAVAILABLE") from exc
    if key_size != 32:
        raise RunnerError("REQUEST_ENVELOPE_KEY_SIZE_INVALID")
    return path


def _stage_sif_owner_key(source: Path, output: Path) -> Path:
    """Create a root-owned exact-SIF copy for a root-launched child.

    MiniNDN needs root for network namespaces, while the application runtime
    deliberately requires its request-envelope key to be owned by the process
    euid.  Keep the operator-owned source as the provenance input and stage
    the same bytes in the root-owned case output only for the SIF child.
    """
    if os.geteuid() != 0:
        return source
    if (not source.is_file() or source.is_symlink()
            or any(parent.is_symlink() for parent in source.parents)):
        raise RunnerError("SIF_ENVELOPE_KEY_SOURCE_INVALID")
    stage_dir = output / ".sif-runtime-inputs"
    if stage_dir.exists() or stage_dir.is_symlink():
        raise RunnerError("SIF_ENVELOPE_KEY_STAGE_REUSED")
    try:
        stage_dir.mkdir(mode=0o700, parents=False, exist_ok=False)
        os.chown(stage_dir, os.geteuid(), os.getegid())
        destination = stage_dir / "request-envelope.key"
        key_bytes = source.read_bytes()
        fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                     0o600)
        try:
            written = os.write(fd, key_bytes)
            if written != len(key_bytes):
                raise OSError("short request-envelope key write")
            os.fchmod(fd, 0o600)
            os.fchown(fd, os.geteuid(), os.getegid())
            os.fsync(fd)
        finally:
            os.close(fd)
    except (OSError, ValueError) as exc:
        raise RunnerError("SIF_ENVELOPE_KEY_STAGE_FAILED") from exc
    return destination


def _ldd_library_map(binary: Path) -> dict[str, Path]:
    """Return the resolved shared libraries reported for *binary*.

    The YOLO case starts a MiniNDN NFD and Python native clients in separate
    processes.  A same-soname, different-build ``libndn-cxx`` is not a valid
    candidate: each process can pass its own import/link check while the NDN
    client later receives a protocol-socket EOF.  Keep this probe small and
    deterministic so it can run before MiniNDN creates any network state.
    """
    try:
        result = subprocess.run(
            ["ldd", str(binary)], capture_output=True, text=True,
            check=False,
        )
    except OSError as exc:
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_PROBE_FAILED:" + str(binary)) from exc
    if result.returncode != 0:
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_PROBE_FAILED:" + str(binary))
    libraries: dict[str, Path] = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(lib[^\s]+)\s+=>\s+(\/[^\s]+)", line)
        if match:
            libraries[match.group(1)] = Path(match.group(2)).resolve()
    return libraries


def _validate_native_library_closure() -> None:
    """Reject a split ndn-cxx/NDN-SVS runtime before NFD is started.

    The native extension, NFD, and the extension's NDN-SVS/NAC-ABE
    dependencies must all resolve the same real ``libndn-cxx`` file.  This is
    a pre-start readiness check, not a substitute for building a sealed SIF;
    it prevents repeating a misleading live run with incompatible host
    libraries.
    """
    nfd = shutil.which("nfd")
    if not nfd:
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_NFD_MISSING")
    expected_abi = f"cpython-{sys.version_info.major}{sys.version_info.minor}"
    extension_candidates = sorted(
        path for path in (ROOT / "pythonWrapper/ndnsf").glob("_ndnsf*.so")
        if expected_abi in path.name
    )
    if not extension_candidates:
        raise RunnerError(
            "NATIVE_LIBRARY_CLOSURE_EXTENSION_MISSING:" + expected_abi)
    extension = extension_candidates[0]
    maps = {
        "extension": _ldd_library_map(extension),
        "nfd": _ldd_library_map(Path(nfd)),
    }
    cxx_name = next((name for name in maps["extension"]
                     if name.startswith("libndn-cxx.so")), None)
    if cxx_name is None:
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_EXTENSION_NDN_CXX_MISSING")
    nfd_cxx_name = next((name for name in maps["nfd"]
                         if name.startswith("libndn-cxx.so")), None)
    if nfd_cxx_name is None:
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_NFD_NDN_CXX_MISSING")
    cxx_paths = {"extension": maps["extension"][cxx_name],
                 "nfd": maps["nfd"][nfd_cxx_name]}
    # Check transitive native dependencies too: otherwise an ndn-svs or NAC-ABE
    # build can silently introduce a second ndn-cxx into the client process.
    for dependency_name in ("libndn-svs.so", "libnac-abe.so"):
        dependency = next((name for name in maps["extension"]
                           if name.startswith(dependency_name)), None)
        if dependency is None:
            continue
        dependency_map = _ldd_library_map(maps["extension"][dependency])
        dependency_cxx = next((name for name in dependency_map
                               if name.startswith("libndn-cxx.so")), None)
        if dependency_cxx is not None:
            cxx_paths["extension->" + dependency_name] = dependency_map[dependency_cxx]
    identities = {label: (str(path), digest_file(path))
                  for label, path in cxx_paths.items()}
    if len(set(identities.values())) != 1:
        detail = ",".join(
            label + "=" + path + "#" + digest
            for label, (path, digest) in sorted(identities.items()))
        raise RunnerError("NATIVE_LIBRARY_CLOSURE_MISMATCH:" + detail)


def _validate_local_native_build(env: Mapping[str, str]) -> None:
    """Bind host-local children to the unified build before starting NFD.

    Exact-SIF children use the existing immutable-candidate closure, never a
    host-built extension. A source checkout must present a fresh local receipt.
    """
    if sif_runtime_enabled() or not (ROOT / ".git").exists():
        return
    helper_spec = importlib.util.spec_from_file_location(
        "spec180_local_build_guard", ROOT / "scripts/spec180_native_build.py")
    if helper_spec is None or helper_spec.loader is None:
        raise RunnerError("LOCAL_NATIVE_BUILD_GUARD_UNAVAILABLE")
    helper = importlib.util.module_from_spec(helper_spec)
    helper_spec.loader.exec_module(helper)
    python = shutil.which("python3", path=env.get("PATH", os.defpath))
    if python is None:
        raise RunnerError("LOCAL_NATIVE_BUILD_PYTHON_MISSING")
    try:
        identity = helper.verify_local_runtime(
            ROOT, python_executable=python, environ=env)
    except helper.IdentityError as exc:
        raise RunnerError("LOCAL_NATIVE_BUILD_REJECTED:" + str(exc)) from exc
    selected = Path(env.get("SPEC180_NATIVE_PROVIDER_BINARY",
                           str(ROOT / "build-system-j2/examples/di-native-provider")))
    if selected.resolve() != Path(identity["provider"]["artifact"]["realpath"]):
        raise RunnerError("LOCAL_NATIVE_BUILD_PROVIDER_COMMAND_MISMATCH")


def _validate_key_map(path: Path) -> dict[str, Path]:
    value = _load_document(path, "offer-public-key-map")
    if not value:
        raise RunnerError("OFFER_KEY_MAP_EMPTY")
    result: dict[str, Path] = {}
    for key_id, key_path in value.items():
        if not isinstance(key_id, str) or not key_id:
            raise RunnerError("OFFER_KEY_ID_INVALID")
        if not isinstance(key_path, str):
            raise RunnerError("OFFER_KEY_PATH_INVALID:" + key_id)
        resolved = _absolute_file(key_path, "offer-public-key:" + key_id)
        if b"PRIVATE KEY" in resolved.read_bytes():
            raise RunnerError("OFFER_KEY_MAP_CONTAINS_PRIVATE_KEY")
        result[key_id] = resolved
    return result


def _validate_catalog_identity(environment: Mapping[str, str]) -> tuple[str, str]:
    catalogue_data_name = environment["SPEC180_YOLO_CATALOG_DATA_NAME"].strip()
    if not _NAME_RE.fullmatch(catalogue_data_name):
        raise RunnerError("CATALOG_DATA_NAME_INVALID")
    catalogue_signer = environment["SPEC180_YOLO_CATALOG_SIGNER"].strip()
    if not _NAME_RE.fullmatch(catalogue_signer):
        raise RunnerError("CATALOG_SIGNER_INVALID")
    # ServiceUser.publishSignedAppData signs only records below the local
    # identity's /NDNSF/DI namespace.  Enforce the same wire naming contract
    # before MiniNDN startup so a future publisher cannot fail after children
    # have been launched or silently publish under an unverifiable name.
    marker = "/NDNSF/DI/"
    if marker not in catalogue_data_name:
        raise RunnerError("CATALOG_DATA_NAME_OUTSIDE_DI_PREFIX")
    prefix, suffix = catalogue_data_name.split(marker, 1)
    if not prefix or not suffix:
        raise RunnerError("CATALOG_DATA_NAME_INVALID")
    if catalogue_signer != prefix:
        raise RunnerError("CATALOG_SIGNER_DATA_PREFIX_MISMATCH")
    return catalogue_data_name, catalogue_signer


def _contains_secret(value: Any) -> bool:
    """Reject secret-bearing names or values anywhere in a metadata object."""
    if isinstance(value, Mapping):
        return any(
            _SECRET_RE.search(str(key)) is not None or _contains_secret(item)
            for key, item in value.items()
        )
    if isinstance(value, (list, tuple, set)):
        return any(_contains_secret(item) for item in value)
    return isinstance(value, str) and _SECRET_RE.search(value) is not None


def _package_file(package: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value or Path(value).is_absolute():
        raise RunnerError("PACKAGE_PATH_INVALID:" + label)
    resolved = (package / value).resolve()
    try:
        resolved.relative_to(package.resolve())
    except ValueError as exc:
        raise RunnerError("PACKAGE_PATH_ESCAPES_ROOT:" + label) from exc
    if not resolved.is_file():
        raise RunnerError("FILE_MISSING:" + label)
    if not os.access(resolved, os.R_OK):
        raise RunnerError("FILE_NOT_READABLE:" + label)
    return resolved


def _validate_package(package: Path, registry: Path) -> Mapping[str, Any]:
    manifest = _load_document(package / "manifest.json", "canonical-manifest")
    if manifest.get("schema") != "spec180-yolo26n-canonical-v1":
        raise RunnerError("CANONICAL_MANIFEST_SCHEMA_UNSUPPORTED")
    if manifest.get("modelFamily") != "YOLO26n":
        raise RunnerError("CANONICAL_MODEL_FAMILY_UNSUPPORTED")
    if manifest.get("providerIndependent") is not True:
        raise RunnerError("CANONICAL_PACKAGE_PROVIDER_BOUND")
    graph = manifest.get("graph")
    weights = manifest.get("weights")
    if not isinstance(graph, Mapping) or not isinstance(weights, Mapping):
        raise RunnerError("CANONICAL_OBJECT_MANIFEST_INCOMPLETE")
    graph_path = _package_file(package, "canonical/yolo26n.onnx", "graph")
    weights_path = _package_file(package, weights.get("path"), "weights")
    if graph.get("graphDigest") != digest_file(graph_path):
        raise RunnerError("CANONICAL_GRAPH_DIGEST_MISMATCH")
    if weights.get("digest") != digest_file(weights_path):
        raise RunnerError("CANONICAL_INITIALIZER_DIGEST_MISMATCH")
    catalogue = manifest.get("catalogue")
    if not isinstance(catalogue, Mapping):
        raise RunnerError("CANONICAL_CATALOGUE_MISSING")
    try:
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
        build_yolo26n_adapter(package, registry_path=registry)
    except Exception as exc:
        raise RunnerError("CANONICAL_CATALOGUE_VERIFY_FAILED") from exc
    encoded = canonical_bytes(manifest)
    if _contains_secret(manifest):
        raise RunnerError("CANONICAL_MANIFEST_SECRET_FIELD")
    del encoded
    return manifest


def _validate_case_config(case: str, config: Mapping[str, Any],
                          topology: Path) -> Mapping[str, Any]:
    """Validate the candidate-bound MiniNDN role and node declaration.

    The policy is an authorization input, not a placement decision.  It must
    be explicit enough for the runner to start the right processes: every
    registered case role is advertised by at least one authorized Provider,
    Providers may advertise more than one role, and each process has a declared
    MiniNDN node.  Runtime placement remains owned by the authenticated ACK
    snapshot after the request is sent.
    """
    services = config.get("services")
    if not isinstance(services, list):
        raise RunnerError("CASE_CONFIG_SERVICES_INVALID")
    inference = [
        item for item in services
        if isinstance(item, Mapping)
        and str(item.get("name", ""))
        and not str(item.get("name", "")).startswith("/NDNSF/DistributedRepo")
    ]
    if len(inference) != 1:
        raise RunnerError("CASE_CONFIG_INFERENCE_SERVICE_AMBIGUOUS")
    service = inference[0]
    roles = service.get("roles")
    if not isinstance(roles, list):
        raise RunnerError("CASE_CONFIG_ROLES_INVALID")
    declared_roles = tuple(str(role) for role in roles if str(role))
    expected_roles = CASE_CONFIG_ROLE_SETS[case]
    if set(declared_roles) != expected_roles or len(declared_roles) != len(expected_roles):
        raise RunnerError("CASE_CONFIG_ROLE_SET_INVALID:" + case)

    providers = service.get("providers")
    if not isinstance(providers, list) or not providers:
        raise RunnerError("CASE_CONFIG_PROVIDERS_INVALID")
    role_capabilities: dict[str, set[str]] = {
        role: set() for role in expected_roles
    }
    provider_identities: set[str] = set()
    provider_roles_by_identity: dict[str, set[str]] = {}
    for provider in providers:
        if not isinstance(provider, Mapping):
            raise RunnerError("CASE_CONFIG_PROVIDER_INVALID")
        identity = str(provider.get("identity", ""))
        if not _NAME_RE.fullmatch(identity):
            raise RunnerError("CASE_CONFIG_PROVIDER_IDENTITY_INVALID")
        if identity in provider_identities:
            raise RunnerError("CASE_CONFIG_PROVIDER_DUPLICATE:" + identity)
        provider_identities.add(identity)
        raw_roles = provider.get("roles")
        if not isinstance(raw_roles, list) or not raw_roles:
            raise RunnerError("CASE_CONFIG_PROVIDER_ROLES_INVALID:" + identity)
        provider_roles = tuple(str(role) for role in raw_roles if str(role))
        if len(provider_roles) != len(set(provider_roles)):
            raise RunnerError("CASE_CONFIG_PROVIDER_ROLE_DUPLICATE:" + identity)
        if any(role not in expected_roles for role in provider_roles):
            raise RunnerError("CASE_CONFIG_PROVIDER_ROLE_UNKNOWN:" + identity)
        provider_roles_by_identity[identity] = set(provider_roles)
        for role in provider_roles:
            role_capabilities[role].add(identity)
    missing_roles = sorted(role for role, providers_for_role
                           in role_capabilities.items()
                           if not providers_for_role)
    if missing_roles:
        raise RunnerError("CASE_CONFIG_ROLE_CAPABILITY_MISSING:" + ",".join(missing_roles))
    expected_provider_count = CASE_PROVIDER_COUNTS[case]
    if len(provider_identities) != expected_provider_count:
        raise RunnerError(
            f"CASE_CONFIG_PROVIDER_COUNT_INVALID:{case}:"
            f"expected={expected_provider_count}")

    def has_distinct_cover(required_roles: set[str]) -> bool:
        """Check that the startup profile can support distinct role owners.

        This is only a capability-profile sanity check.  It does not select
        or bind a Provider; the authenticated ACK snapshot remains the sole
        request-time placement authority.
        """
        matched: dict[str, str] = {}

        def visit(role: str, seen: set[str]) -> bool:
            for identity in sorted(provider_roles_by_identity):
                if role not in provider_roles_by_identity[identity] or identity in seen:
                    continue
                seen.add(identity)
                previous = matched.get(identity)
                if previous is None or visit(previous, seen):
                    matched[identity] = role
                    return True
            return False

        return all(visit(role, set()) for role in sorted(required_roles))

    if (case == "Y-A" and
            provider_roles_by_identity[sorted(provider_identities)[0]] !=
            {"FullModel"}):
        raise RunnerError("CASE_CONFIG_Y_A_PROVIDER_PROFILE_INVALID")
    if case in ("Y-B", "Y-N") and not has_distinct_cover(
            {"BackboneNeck", "DetectShard0", "DetectShard1", "Merge"}):
        raise RunnerError("CASE_CONFIG_DISTINCT_SHARED_COVERAGE_INVALID:" + case)
    if case == "Y-N" and not any(
            "FullModel" in roles for roles in provider_roles_by_identity.values()):
        raise RunnerError("CASE_CONFIG_ATOMIC_COVERAGE_INVALID:Y-N")
    runtime = config.get("runtime")
    if not isinstance(runtime, Mapping):
        raise RunnerError("CASE_CONFIG_RUNTIME_INVALID")
    nodes = runtime.get("nodes")
    if not isinstance(nodes, Mapping):
        raise RunnerError("CASE_CONFIG_MININDN_NODES_MISSING")
    for key in ("controller", "user", "repo"):
        value = str(nodes.get(key, ""))
        if not value:
            raise RunnerError("CASE_CONFIG_NODE_MISSING:" + key)
    provider_nodes = nodes.get("providers")
    if not isinstance(provider_nodes, Mapping):
        raise RunnerError("CASE_CONFIG_PROVIDER_NODES_MISSING")
    for identity in provider_identities:
        node = str(provider_nodes.get(identity, ""))
        if not node:
            raise RunnerError("CASE_CONFIG_PROVIDER_NODE_MISSING:" + identity)
    identities = runtime.get("identities")
    if not isinstance(identities, Mapping):
        raise RunnerError("CASE_CONFIG_IDENTITIES_MISSING")
    for key in ("controller", "user", "repo", "group"):
        value = str(identities.get(key, ""))
        if not _NAME_RE.fullmatch(value):
            raise RunnerError("CASE_CONFIG_IDENTITY_INVALID:" + key)
    if str(config.get("controller", "")) != str(identities["controller"]):
        raise RunnerError("CASE_CONFIG_CONTROLLER_IDENTITY_MISMATCH")
    if str(config.get("group", "")) != str(identities["group"]):
        raise RunnerError("CASE_CONFIG_GROUP_IDENTITY_MISMATCH")
    if str(runtime.get("user_identity", "")) != str(identities["user"]):
        raise RunnerError("CASE_CONFIG_USER_IDENTITY_MISMATCH")
    provider_prefix = str(identities.get("providerPrefix", ""))
    if not _NAME_RE.fullmatch(provider_prefix):
        raise RunnerError("CASE_CONFIG_IDENTITY_INVALID:providerPrefix")
    if str(runtime.get("provider_prefix", "")) != provider_prefix:
        raise RunnerError("CASE_CONFIG_PROVIDER_PREFIX_MISMATCH")
    identity_providers = identities.get("providers")
    if not isinstance(identity_providers, Mapping):
        raise RunnerError("CASE_CONFIG_PROVIDER_IDENTITIES_MISSING")
    if set(identity_providers) != provider_identities:
        raise RunnerError("CASE_CONFIG_PROVIDER_IDENTITY_SET_MISMATCH")
    for identity in provider_identities:
        advertised = str(identity_providers.get(identity, ""))
        if advertised != identity or not _NAME_RE.fullmatch(advertised):
            raise RunnerError("CASE_CONFIG_PROVIDER_IDENTITY_INVALID:" + identity)
    if any(identity != provider_prefix and
           not identity.startswith(provider_prefix + "/")
           for identity in provider_identities):
        raise RunnerError("CASE_CONFIG_PROVIDER_PREFIX_MISMATCH")
    repo_service_prefix = str(
        identities.get("repoServicePrefix", "/NDNSF/DistributedRepo/Object"))
    if not _NAME_RE.fullmatch(repo_service_prefix):
        raise RunnerError("CASE_CONFIG_IDENTITY_INVALID:repoServicePrefix")
    try:
        topology_text = topology.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise RunnerError("TOPOLOGY_READ_FAILED") from exc
    declared_nodes = {
        str(nodes[key]) for key in ("controller", "user", "repo")
    }
    declared_nodes.update(str(provider_nodes[identity]) for identity in provider_identities)
    if any(not node or re.search(rf"(?m)^\s*{re.escape(node)}\s*:", topology_text) is None
           for node in declared_nodes):
        raise RunnerError("CASE_CONFIG_NODE_NOT_IN_TOPOLOGY")
    return {
        "serviceName": str(service.get("name")),
        "providerCount": len(provider_identities),
        "requiredProviderCount": CASE_PROVIDER_COUNTS[case],
        "providerIdentities": sorted(provider_identities),
        "distinctCapabilityCover": case in ("Y-B", "Y-N"),
        "roleCapabilities": {
            role: sorted(providers_for_role)
            for role, providers_for_role in sorted(role_capabilities.items())
        },
        "nodes": {
            "controller": str(nodes["controller"]),
            "user": str(nodes["user"]),
            "repo": str(nodes["repo"]),
            "providers": {
                identity: str(provider_nodes[identity])
                for identity in sorted(provider_identities)
            },
        },
        "identities": {
            "controller": str(identities["controller"]),
            "user": str(identities["user"]),
            "repo": str(identities["repo"]),
            "group": str(identities["group"]),
            "providerPrefix": provider_prefix,
            "repoServicePrefix": repo_service_prefix,
            "providers": {
                identity: str(identity_providers[identity])
                for identity in sorted(provider_identities)
            },
        },
    }


def _validate_policy_loader_compatibility(config: Mapping[str, Any]) -> None:
    """Run the maintained policy checks before any MiniNDN side effect.

    The case-specific checks protect the fixed Spec180 profile, but they do
    not replace the application's policy loader.  This read-only preflight
    catches missing user authorization, malformed service descriptors, role
    coverage, and known runtime-compatibility errors before NFD or a child
    process is started.  Policy generation and certificate installation stay
    in the later live driver.
    """
    try:
        sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
        from ndnsf_distributed_inference.policy import (
            parse_services,
            validate_runtime_user_authorization,
            validate_service_provider_role_coverage,
            validate_service_runtime_compatibility,
        )
        config_dict = dict(config)
        services = parse_services(config_dict)
        validate_runtime_user_authorization(config_dict, services)
        validate_service_provider_role_coverage(services)
        validate_service_runtime_compatibility(services)
    except Exception as exc:
        raise RunnerError(
            "CASE_CONFIG_POLICY_LOADER_INVALID:" + type(exc).__name__ + ":" +
            str(exc)) from exc


def _materialize_case_config(case: str, config: Mapping[str, Any],
                             output: Path) -> Path:
    """Write an isolated case policy without changing the caller's policy.

    The resulting policy is a process-start authorization view.  It contains
    only the roles and explicitly authorized Providers for this registered
    case; it does not preselect a Provider for a request.
    """
    services = config.get("services")
    if not isinstance(services, list):
        raise RunnerError("CASE_CONFIG_SERVICES_INVALID")
    copied = json.loads(json.dumps(config, ensure_ascii=False))
    inference = [
        item for item in copied["services"]
        if isinstance(item, Mapping)
        and not str(item.get("name", "")).startswith("/NDNSF/DistributedRepo")
    ]
    if len(inference) != 1:
        raise RunnerError("CASE_CONFIG_INFERENCE_SERVICE_AMBIGUOUS")
    service = inference[0]
    expected = CASE_CONFIG_ROLE_SETS[case]
    service["roles"] = sorted(expected)
    filtered_providers = []
    for provider in service.get("providers", []):
        if not isinstance(provider, Mapping):
            raise RunnerError("CASE_CONFIG_PROVIDER_INVALID")
        roles = [str(role) for role in provider.get("roles", [])
                 if str(role) in expected]
        if roles:
            filtered = dict(provider)
            filtered["roles"] = roles
            filtered_providers.append(filtered)
    if {str(role) for item in filtered_providers for role in item["roles"]} != expected:
        raise RunnerError("CASE_CONFIG_ISOLATED_ROLE_COVERAGE_INVALID")
    service["providers"] = sorted(
        filtered_providers, key=lambda item: str(item.get("identity", "")))
    # RepoNodeApp registers every canonical public/internal repository service
    # during construction and cannot become ready unless the controller policy
    # authorizes its Provider identity for that complete set.  Materialize the
    # infrastructure services deterministically here so an otherwise valid
    # inference-only case cannot hang at repository permission discovery.
    from py_repoclient.service_names import repo_versioned_services

    runtime = copied.get("runtime", {})
    identities = runtime.get("identities", {}) if isinstance(runtime, Mapping) else {}
    repo_identity = str(identities.get("repo", ""))
    controller_identity = str(identities.get("controller", ""))
    user_identity = str(identities.get("user", ""))
    provider_identities = {
        str(item.get("identity", "")) for item in filtered_providers
    }
    repo_clients = sorted({
        controller_identity, user_identity, repo_identity, *provider_identities,
    })
    if any(not _NAME_RE.fullmatch(identity) for identity in repo_clients):
        raise RunnerError("CASE_CONFIG_REPO_AUTHORIZATION_IDENTITY_INVALID")
    copied["services"] = [
        item for item in copied["services"]
        if not (isinstance(item, Mapping) and
                str(item.get("name", "")).startswith("/NDNSF/DistributedRepo/"))
    ]
    copied["services"].extend({
        "name": name,
        "model": name,
        "roles": [],
        "users": repo_clients,
        "providers": [{"identity": repo_identity, "roles": []}],
        "dependencies": [],
    } for name in repo_versioned_services())
    policy_path = output / "case-policy.json"
    policy_path.write_text(
        json.dumps(copied, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    return policy_path


def _build_case_plan(case: str, manifest: Mapping[str, Any],
                     package_manifest_digest: str) -> Mapping[str, Any]:
    """Materialize candidate-family expectations without assigning Providers.

    This file is a preflight plan, not a placement result.  Provider names and
    role assignments must still come from the authenticated ACK snapshot after
    the live request is sent.
    """
    if case not in CASE_CANDIDATES:
        raise RunnerError("CASE_NOT_REGISTERED:" + case)
    catalogue = manifest.get("catalogue")
    rows = catalogue.get("candidates") if isinstance(catalogue, Mapping) else None
    if not isinstance(rows, list):
        raise RunnerError("CASE_CATALOGUE_MISSING")
    by_id = {
        str(row.get("candidateId", "")): row
        for row in rows if isinstance(row, Mapping)
    }
    plans = []
    for candidate_id in CASE_CANDIDATES[case]:
        row = by_id.get(candidate_id)
        if row is None:
            raise RunnerError("CASE_CANDIDATE_MISSING:" + candidate_id)
        roles = row.get("roles")
        if not isinstance(roles, list):
            raise RunnerError("CASE_CANDIDATE_ROLES_INVALID:" + candidate_id)
        role_names = tuple(str(item.get("role", "")) for item in roles
                           if isinstance(item, Mapping))
        if role_names != CASE_ROLE_SETS[candidate_id]:
            raise RunnerError("CASE_CANDIDATE_ROLE_SET_INVALID:" + candidate_id)
        candidate_digest = str(row.get("candidateDigest", ""))
        if not _DIGEST_RE.fullmatch(candidate_digest):
            raise RunnerError("CASE_CANDIDATE_DIGEST_INVALID:" + candidate_id)
        ingress = str(row.get("inputIngressRole", ""))
        egress = str(row.get("resultEgressRole", ""))
        if ingress not in role_names or egress not in role_names:
            raise RunnerError("CASE_CANDIDATE_OWNERSHIP_INVALID:" + candidate_id)
        plans.append({
            "candidateId": candidate_id,
            "candidateDigest": candidate_digest,
            "selectionPriority": int(row.get("selectionPriority", -1)),
            "roles": list(role_names),
            "inputIngressRole": ingress,
            "resultEgressRole": egress,
            "mergeKind": str(row.get("mergeKind", "")),
        })
    result = {
        "schema": "spec180-yolo-case-plan-v1",
        "case": case,
        "packageManifestSha256": package_manifest_digest,
        "candidateAuthority": "ACK_SNAPSHOT_ONLY",
        "providerAssignment": "RUNTIME_ACK_PROJECTION_REQUIRED",
        "candidatePlans": plans,
    }
    if case == "Y-N":
        result["subcases"] = [
            {"id": subcase, "expected": "CONTROL" if subcase == "Y-N-O"
             else "FAIL_CLOSED", "boundary": YN_SUBCASE_BOUNDARIES[subcase]}
            for subcase in YN_SUBCASES
        ]
    if _contains_secret(result):
        raise RunnerError("CASE_PLAN_SECRET_FIELD")
    return result


def build_runtime_catalogue_payload(
        case: str,
        data_name: str,
        manifest: Mapping[str, Any],
        snapshots: tuple[Any, ...] | list[Any],
    ) -> bytes:
    """Build the candidate-bound active catalogue APP payload.

    The package manifest supplies the required candidate identities; the
    snapshots supply already-published role/rank Data names.  Keeping this
    composition in the runner makes the publication barrier explicit while
    leaving signing and transport to the controller-owned ServiceUser path.
    """
    if case not in CASE_CANDIDATES:
        raise RunnerError("CASE_NOT_REGISTERED:" + str(case))
    catalogue = manifest.get("catalogue") if isinstance(manifest, Mapping) else None
    rows = catalogue.get("candidates") if isinstance(catalogue, Mapping) else None
    if not isinstance(rows, list):
        raise RunnerError("CASE_CATALOGUE_MISSING")
    by_id = {
        str(row.get("candidateId", "")): row
        for row in rows if isinstance(row, Mapping)
    }
    required_ids = CASE_CANDIDATES[case]
    required_digests = []
    for candidate_id in required_ids:
        row = by_id.get(candidate_id)
        digest = str(row.get("candidateDigest", "")) if row else ""
        if not _DIGEST_RE.fullmatch(digest):
            raise RunnerError("CASE_CANDIDATE_DIGEST_INVALID:" + candidate_id)
        required_digests.append(digest)
    if len(set(required_digests)) != len(required_digests):
        raise RunnerError("CASE_CANDIDATE_DIGEST_DUPLICATE")
    required_set = set(required_digests)
    snapshots_by_candidate: dict[str, list[Any]] = {}
    for snapshot in snapshots:
        candidate_digest = str(getattr(snapshot, "candidate_digest", ""))
        if candidate_digest not in required_set:
            raise RunnerError("CASE_RUNTIME_SNAPSHOT_CANDIDATE_UNEXPECTED")
        snapshots_by_candidate.setdefault(candidate_digest, []).append(snapshot)
    for candidate_id, candidate_digest in zip(required_ids, required_digests):
        matching = snapshots_by_candidate.get(candidate_digest, [])
        if len(matching) != 1:
            raise RunnerError(
                "CASE_RUNTIME_SNAPSHOT_CANDIDATE_COVERAGE:" + candidate_id)
        snapshot = matching[0]
        expected_roles = set(CASE_ROLE_SETS[candidate_id])
        artifact_names = getattr(snapshot, "artifact_data_names", None)
        if not isinstance(artifact_names, Mapping):
            raise RunnerError(
                "CASE_RUNTIME_SNAPSHOT_ARTIFACT_COVERAGE:" + candidate_id)
        if set(str(role) for role in artifact_names) != expected_roles:
            raise RunnerError(
                "CASE_RUNTIME_SNAPSHOT_ARTIFACT_COVERAGE:" + candidate_id)
        all_names: list[str] = []
        for role in sorted(expected_roles):
            names = artifact_names.get(role)
            if (not isinstance(names, (list, tuple)) or not names
                    or any(not isinstance(name, str)
                           or not _NAME_RE.fullmatch(name)
                           for name in names)):
                raise RunnerError(
                    "CASE_RUNTIME_SNAPSHOT_ARTIFACT_NAME_INVALID:" + candidate_id)
            if len(set(names)) != len(names):
                raise RunnerError(
                    "CASE_RUNTIME_SNAPSHOT_ARTIFACT_DUPLICATE:" + candidate_id)
            all_names.extend(names)
        if len(set(all_names)) != len(all_names):
            raise RunnerError(
                "CASE_RUNTIME_SNAPSHOT_ARTIFACT_DUPLICATE:" + candidate_id)
        if not str(getattr(snapshot, "backend", "")) or not str(
                getattr(snapshot, "precision", "")):
            raise RunnerError(
                "CASE_RUNTIME_SNAPSHOT_RUNTIME_IDENTITY:" + candidate_id)
    try:
        from ndnsf_distributed_inference.app_sdk.placement import (
            encode_runtime_catalog_snapshot,
        )
        return encode_runtime_catalog_snapshot(
            data_name, list(snapshots),
            required_candidate_digests=tuple(required_digests))
    except RunnerError:
        raise
    except (TypeError, ValueError) as exc:
        raise RunnerError("CASE_RUNTIME_CATALOGUE_INVALID:" + str(exc)) from exc


def _manifest_model_identity(manifest: Mapping[str, Any]) -> tuple[str, str, str]:
    """Return the model, graph, and preprocessing identity from one package."""
    source = manifest.get("source")
    graph = manifest.get("graph")
    if not isinstance(source, Mapping) or not isinstance(graph, Mapping):
        raise RunnerError("CANONICAL_MANIFEST_MODEL_IDENTITY_MISSING")
    model_digest = str(source.get("checkpointSha256", ""))
    if not model_digest.startswith("sha256:"):
        model_digest = "sha256:" + model_digest
    graph_digest = str(graph.get("graphDigest", ""))
    if not _DIGEST_RE.fullmatch(model_digest) or not _DIGEST_RE.fullmatch(graph_digest):
        raise RunnerError("CANONICAL_MANIFEST_MODEL_IDENTITY_INVALID")
    semantics_digest = digest_bytes(canonical_bytes({
        "preprocessing": manifest.get("preprocessing", {}),
        "postprocessing": manifest.get("postprocessing", {}),
    }))
    return model_digest, graph_digest, semantics_digest


def build_runtime_publication_file(
        binding: CaseRuntimeBinding, inputs: Mapping[str, Any]) -> Path:
    """Materialize the controller-owned publication batch for one case.

    The parent runner never signs or publishes APP Data from the host process:
    it writes a candidate-bound batch, and the Controller child performs the
    signing, publication, and exact readback inside the MiniNDN namespace.
    Artifact records are intentionally small signed metadata objects for the
    first Y-A vertical slice; the selected Provider uses its validated local
    canonical ONNX package.  Later Y-B publication may replace these records
    with encrypted large-data references without changing the barrier.
    """
    package = inputs.get("package")
    manifest = inputs.get("manifest")
    if not isinstance(package, Path) or not isinstance(manifest, Mapping):
        raise RunnerError("CASE_RUNTIME_PUBLICATION_INPUT_INCOMPLETE")
    controller = str(binding.identities["controller"]).rstrip("/")
    data_name = str(inputs.get("descriptor", {}).get("catalogueDataName", ""))
    signer = str(inputs.get("descriptor", {}).get("catalogueSigner", ""))
    if not (_NAME_RE.fullmatch(data_name) and signer == controller):
        raise RunnerError("CASE_RUNTIME_PUBLICATION_IDENTITY_INVALID")
    model_digest, graph_digest, semantics_digest = _manifest_model_identity(manifest)
    manifest_digest = digest_file(package / "manifest.json")
    rows = manifest.get("catalogue", {}).get("candidates", [])
    by_id = {
        str(row.get("candidateId", "")): row for row in rows
        if isinstance(row, Mapping)
    }
    # The coordinator derives each SplitCandidate digest at runtime from the
    # adapter (role set, ingress/egress ownership, fragments, graph).  The raw
    # catalogue row digest cannot match the selected candidate, which would
    # force the GENERATED split path that the maintained case rejects.  Publish
    # the runtime digests computed through the same adapter construction.
    registry_path = inputs.get("registry")
    if not isinstance(registry_path, (str, Path)):
        raise RunnerError("CASE_RUNTIME_PUBLICATION_REGISTRY_MISSING")
    try:
        from ndnsf_distributed_inference.adapters.yolo import (
            build_yolo26n_adapter)
        adapter = build_yolo26n_adapter(package, registry_path=str(registry_path))
        runtime_model = adapter.describe_model(
            "YOLO26n", model_digest, semantics_digest,
            source_revision=str(manifest.get("graphRevision", "")))
        runtime_graph = adapter.graph.inspect(runtime_model)
        runtime_digest_by_role_set = {
            tuple(item.execution_plan.roles): item.candidate_digest
            for item in adapter.splitter.enumerate_candidates(
                runtime_model, runtime_graph)
        }
    except (ImportError, TypeError, ValueError) as exc:
        raise RunnerError("CASE_RUNTIME_PUBLICATION_RUNTIME_IDENTITY_FAILED") from exc
    candidate_ids = CASE_CANDIDATES[binding.case]
    # Y-N-O deliberately reverses the signed runtime snapshot input order.
    # The wire permutation is applied after canonical encoding below: the
    # resolver must still verify the canonical snapshotDigest after sorting,
    # while the signed bytes visibly exercise order independence.
    snapshots: list[dict[str, Any]] = []
    artifacts: list[dict[str, str]] = []
    for candidate_id in candidate_ids:
        row = by_id.get(candidate_id)
        row_digest = str(row.get("candidateDigest", "")) if row else ""
        if not _DIGEST_RE.fullmatch(row_digest):
            raise RunnerError("CASE_RUNTIME_PUBLICATION_CANDIDATE_INVALID:" + candidate_id)
        candidate_digest = runtime_digest_by_role_set.get(
            CASE_ROLE_SETS[candidate_id], "")
        if not _DIGEST_RE.fullmatch(candidate_digest):
            raise RunnerError(
                "CASE_RUNTIME_PUBLICATION_RUNTIME_DIGEST_INVALID:" + candidate_id)
        roles = CASE_ROLE_SETS[candidate_id]
        root = f"{controller}/NDNSF/DI/ARTIFACT/{candidate_digest[7:]}"
        names = {role: (f"{root}/{role}",) for role in roles}
        snapshots.append({
            "alias": candidate_id,
            "manifestDigest": manifest_digest,
            "modelContentDigest": model_digest,
            "semanticsDigest": semantics_digest,
            # The coordinator binds the adapter's runtime graph identity, not
            # the ONNX file digest recorded in the package manifest.
            "graphDigest": runtime_graph.graph_digest,
            "candidateDigest": candidate_digest,
            "backend": "onnxruntime-cpu",
            "precision": "float32",
            "artifactDataNames": names,
            "status": "ACTIVE",
            "createdAtMs": int(time.time() * 1000),
        })
        for role, values in names.items():
            for artifact_name in values:
                payload = canonical_bytes({
                    "schema": "spec180-yolo-execution-artifact-v1",
                    "candidateDigest": candidate_digest,
                    "graphDigest": graph_digest,
                    "role": role,
                    "backend": "onnxruntime-cpu",
                    "precision": "float32",
                })
                artifacts.append({
                    "dataName": artifact_name,
                    "payloadB64": base64.b64encode(payload).decode("ascii"),
                    "payloadDigest": digest_bytes(payload),
                })
    try:
        from ndnsf_distributed_inference.app_sdk.contracts import PreSplitCatalogSnapshot
        from ndnsf_distributed_inference.app_sdk.placement import encode_runtime_catalog_snapshot
        typed = [PreSplitCatalogSnapshot.from_mapping(item) for item in snapshots]
        catalogue_payload = encode_runtime_catalog_snapshot(
            data_name, typed,
            required_candidate_digests=tuple(
                runtime_digest_by_role_set[CASE_ROLE_SETS[item]]
                for item in candidate_ids))
        if str(inputs.get("subcase", "")) == "Y-N-O":
            envelope = json.loads(catalogue_payload.decode("utf-8"))
            envelope["snapshots"] = list(reversed(envelope["snapshots"]))
            catalogue_payload = canonical_bytes(envelope)
    except (TypeError, ValueError, KeyError) as exc:
        raise RunnerError("CASE_RUNTIME_PUBLICATION_SNAPSHOT_INVALID") from exc
    publication = {
        "schema": "spec180-runtime-publication-v1",
        "case": binding.case,
        "catalogueDataName": data_name,
        "catalogueSigner": signer,
        "cataloguePayloadB64": base64.b64encode(catalogue_payload).decode("ascii"),
        "cataloguePayloadDigest": digest_bytes(catalogue_payload),
        "artifacts": artifacts,
        "packageManifestSha256": manifest_digest,
    }
    path = binding.output / "runtime-publication.json"
    path.write_text(json.dumps(publication, ensure_ascii=False,
                               sort_keys=True, indent=2) + "\n",
                    encoding="utf-8")
    return path


def validate_inputs(case: str, environment: Mapping[str, str]) -> tuple[Path, Mapping[str, Any]]:
    """Validate all candidate-bound inputs before MiniNDN is started."""
    if case not in CASE_IDS:
        raise RunnerError("CASE_NOT_REGISTERED:" + case)
    missing = [name for name in REQUIRED_ENV if not environment.get(name)]
    if missing:
        raise RunnerError("ENVIRONMENT_MISSING:" + ",".join(missing))
    output = _validate_output_root(environment.get("SPEC180_CASE_OUTPUT_DIR", ""))
    state_root = _validate_state_root(environment["NDNSF_DI_STATE_ROOT"])
    envelope_key_file = _validate_envelope_key_file(
        environment["NDNSF_DI_ENVELOPE_KEY_FILE"])
    package = _absolute_directory(environment["SPEC180_YOLO_CANONICAL_PACKAGE"],
                                  "canonical-package")
    registry = _absolute_file(environment["SPEC180_YOLO_CATALOGUE_REGISTRY"],
                              "catalogue-registry")
    _validate_package(package, registry)
    key_map_path = _absolute_file(
        environment["SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP"],
        "offer-public-key-map")
    key_map = _validate_key_map(key_map_path)
    trust_root = _absolute_file(environment["SPEC180_YOLO_OFFER_TRUST_ROOT"],
                                "offer-trust-root")
    trust_root_doc = _load_document(trust_root, "offer-trust-root")
    if trust_root_doc.get("schema") != "spec180-provider-offer-trust-v1":
        raise RunnerError("OFFER_TRUST_ROOT_SCHEMA_UNSUPPORTED")
    catalogue_data_name, catalogue_signer = _validate_catalog_identity(environment)
    topology = _absolute_file(environment["SPEC180_YOLO_TOPOLOGY"], "topology")
    config = _absolute_file(environment["SPEC180_YOLO_CONFIG"], "config")
    config_doc = _load_document(config, "config")
    if not topology.read_text(encoding="utf-8").strip():
        raise RunnerError("TOPOLOGY_EMPTY")
    config_wire = json.dumps(config_doc, ensure_ascii=False, sort_keys=True)
    if "/Stage/" in config_wire or "selection-offer-key-map" in config_wire:
        raise RunnerError("LEGACY_DEPLOYMENT_POLICY_FORBIDDEN")
    if re.search(r"(?i)hmac|caller[_ -]?key", config_wire):
        raise RunnerError("CALLER_HMAC_POLICY_FORBIDDEN")
    _validate_policy_loader_compatibility(config_doc)
    case_runtime = _validate_case_config(case, config_doc, topology)
    if catalogue_signer != str(case_runtime["identities"]["controller"]):
        raise RunnerError("CATALOG_SIGNER_CONTROLLER_MISMATCH")
    private_key_map_path = _absolute_file(
        environment["SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP"],
        "offer-private-key-map")
    private_key_map_doc = _load_document(
        private_key_map_path, "offer-private-key-map")
    if not isinstance(private_key_map_doc, Mapping):
        raise RunnerError("OFFER_PRIVATE_KEY_MAP_INVALID")
    private_key_file_digests: dict[str, str] = {}
    for provider_identity in case_runtime["providerIdentities"]:
        key_value = private_key_map_doc.get(provider_identity)
        if (not isinstance(key_value, str) or not key_value
                or not Path(key_value).expanduser().is_absolute()):
            raise RunnerError("OFFER_PRIVATE_KEY_MISSING:" + provider_identity)
        key_path = Path(key_value).expanduser()
        if not key_path.is_file() or not os.access(key_path, os.R_OK):
            raise RunnerError("OFFER_PRIVATE_KEY_INVALID:" + provider_identity)
        # Bind secret-bearing key files by digest only.  The private bytes stay
        # outside the descriptor, while a later process cannot silently swap
        # the key map or one Provider key after this preflight.
        private_key_file_digests[provider_identity] = digest_file(key_path)
    case_policy = _materialize_case_config(case, config_doc, output)
    manifest = _load_document(package / "manifest.json", "canonical-manifest")
    catalogue = manifest.get("catalogue", {})
    candidates = catalogue.get("candidates", []) if isinstance(catalogue, Mapping) else []
    candidate_ids = {str(item.get("candidateId", "")) for item in candidates
                     if isinstance(item, Mapping)}
    expected = {
        "Y-A": {"atomic-v1"},
        "Y-B": {"shared-backbone-two-shard-v1"},
        "Y-N": {"atomic-v1", "shared-backbone-two-shard-v1"},
    }[case]
    if not expected.issubset(candidate_ids):
        raise RunnerError("CASE_CANDIDATE_SET_INCOMPLETE:" + case)
    package_manifest_digest = digest_file(package / "manifest.json")
    descriptor = {
        "schema": "spec180-yolo-case-input-v1",
        "case": case,
        "packageManifestSha256": package_manifest_digest,
        "catalogueRegistrySha256": digest_file(registry),
        "catalogueDataName": catalogue_data_name,
        "catalogueSigner": catalogue_signer,
        "offerTrustRootSha256": digest_file(trust_root),
        "offerPublicKeyMapSha256": digest_file(key_map_path),
        "offerPublicKeysSha256": {
            key_id: digest_file(key_path)
            for key_id, key_path in sorted(key_map.items())
        },
        "offerPrivateKeyMapSha256": digest_file(private_key_map_path),
        "offerPrivateKeysSha256": private_key_file_digests,
        "canonicalModelRelativePath": "canonical/yolo26n.onnx",
        "canonicalModelSha256": digest_file(package / "canonical/yolo26n.onnx"),
        "topologySha256": digest_file(topology),
        "configSha256": digest_file(config),
        "stateRoot": str(state_root),
        "requestEnvelopeKeySha256": digest_file(envelope_key_file),
        "candidateIds": sorted(expected),
        "providerIndependent": True,
        "caseRuntime": case_runtime,
        "casePolicySha256": digest_file(case_policy),
    }
    (output / "case-input.json").write_text(
        json.dumps(descriptor, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    case_plan = _build_case_plan(case, manifest, package_manifest_digest)
    (output / "case-plan.json").write_text(
        json.dumps(case_plan, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
        encoding="utf-8",
    )
    return output, {"package": package, "registry": registry,
                    "offer_trust_root": trust_root,
                    "offer_public_key_map": key_map_path,
                    "offer_private_key_map": private_key_map_path,
                    "topology": topology, "config": config,
                    "manifest": manifest, "descriptor": descriptor,
                    "case_plan": case_plan, "case_policy": case_policy,
                    "config_doc": config_doc, "state_root": state_root,
                    "envelope_key_file": envelope_key_file}


def validate_runtime_publication_receipt(expected: Mapping[str, Any],
                                          receipt: Mapping[str, Any]) -> Mapping[str, Any]:
    """Retain the existing runner error contract around the shared pure checker."""
    from Experiments.TigerCluster.runtime.yolo_result import (
        EvidenceError, validate_runtime_publication_receipt as validate)
    try:
        return validate(expected, receipt)
    except EvidenceError as exc:
        raise RunnerError(str(exc)) from exc


def _wait_for_runtime_receipt(output: Path, publication: Path,
                              timeout_s: float = 20.0) -> Mapping[str, Any]:
    """Wait for the Controller child's signed APP publication receipt."""
    if timeout_s <= 0:
        raise RunnerError("CASE_RUNTIME_PUBLICATION_TIMEOUT_INVALID")
    try:
        expected = json.loads(publication.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RunnerError("CASE_RUNTIME_PUBLICATION_FILE_INVALID") from exc
    receipt_path = publication.with_name("runtime-publication-receipt.json")
    deadline = time.monotonic() + float(timeout_s)
    while time.monotonic() < deadline:
        if receipt_path.exists():
            try:
                receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise RunnerError("CASE_RUNTIME_PUBLICATION_RECEIPT_INVALID") from exc
            return validate_runtime_publication_receipt(expected, receipt)
        time.sleep(0.1)
    raise RunnerError("CASE_RUNTIME_PUBLICATION_RECEIPT_TIMEOUT")


class LifecycleJournal:
    """Write exactly-once non-secret lifecycle events for one request."""

    def __init__(self, output: Path, case: str, *,
                 require_protocol_binding: bool = True) -> None:
        self.case = case
        self.request_id = "spec180-" + hashlib.sha256(
            f"{case}:{time.time_ns()}".encode()).hexdigest()[:16]
        self.attempt_id = "attempt-" + hashlib.sha256(
            f"{self.request_id}:{time.time_ns()}".encode()).hexdigest()[:16]
        self._require_protocol_binding = bool(require_protocol_binding)
        self._protocol_bound = False
        self._index = 0
        self._seen: set[str] = set()
        self._path = output / "lifecycle.jsonl"

    @property
    def path(self) -> Path:
        return self._path

    @property
    def last_milestone(self) -> str:
        return MILESTONES[self._index - 1] if self._index else ""

    @staticmethod
    def _validate_identity(value: str, label: str) -> str:
        if not isinstance(value, str) or not _IDENTITY_RE.fullmatch(value):
            raise RunnerError("LIFECYCLE_PROTOCOL_IDENTITY_INVALID:" + label)
        return value

    @staticmethod
    def _validate_request_id(value: str) -> str:
        # Native NDNSF exposes a request identifier as an absolute NDN Name.
        # Retain opaque identifiers for isolated journal fixtures, but do not
        # force production Names through the narrower attempt-ID grammar.
        if (not isinstance(value, str)
                or not (_NAME_RE.fullmatch(value)
                        or _IDENTITY_RE.fullmatch(value))):
            raise RunnerError("LIFECYCLE_PROTOCOL_IDENTITY_INVALID:requestId")
        return value

    def bind_protocol_identity(self, *, request_id: str, attempt_id: str) -> None:
        """Bind the journal to IDs returned by the live coordinator/ACK path.

        A live case must not manufacture lineage in the evidence writer.  The
        runner may create provisional IDs for isolated journal unit tests, but
        the production driver calls this method before the first milestone.
        Rebinding or binding after an event would make the trace ambiguous and
        is therefore rejected.
        """
        if self._index or self._protocol_bound:
            raise RunnerError("LIFECYCLE_PROTOCOL_IDENTITY_REBIND")
        self.request_id = self._validate_request_id(request_id)
        self.attempt_id = self._validate_identity(attempt_id, "attemptId")
        self._protocol_bound = True

    def bind_coordinator_handle(self, handle: Any, *, attempt_id: str) -> None:
        """Bind an actual coordinator handle before the first event.

        The request identity must come from the returned collaboration handle;
        the attempt identity is supplied by the driver from the protocol
        attempt/ACK snapshot.  Neither value is generated or inferred here.
        """
        request_id = str(getattr(handle, "request_id", "") or
                         getattr(getattr(handle, "collaboration", None),
                                 "request_id", ""))
        if not request_id:
            raise RunnerError("LIFECYCLE_COORDINATOR_REQUEST_ID_MISSING")
        self.bind_protocol_identity(request_id=request_id,
                                    attempt_id=str(attempt_id))

    def append(self, milestone: str, **fields: Any) -> None:
        if self._require_protocol_binding and not self._protocol_bound:
            raise RunnerError("LIFECYCLE_PROTOCOL_IDENTITY_UNBOUND")
        if milestone not in MILESTONES:
            raise RunnerError("LIFECYCLE_MILESTONE_UNKNOWN:" + milestone)
        if milestone in self._seen:
            raise RunnerError("LIFECYCLE_DUPLICATE:" + milestone)
        expected = MILESTONES[self._index]
        if milestone != expected:
            raise RunnerError(f"LIFECYCLE_OUT_OF_ORDER:{milestone}:expected={expected}")
        allowed = _LIFECYCLE_FIELD_ALLOWLIST[milestone]
        for key, value in fields.items():
            if _SECRET_RE.search(str(key)) or _SECRET_RE.search(str(value)):
                raise RunnerError("LIFECYCLE_SECRET_FIELD")
            if key not in allowed or _FORBIDDEN_LIFECYCLE_FIELD_RE.search(str(key)):
                raise RunnerError("LIFECYCLE_FIELD_FORBIDDEN:" + str(key))
            # Do not allow nested objects or byte-like values.  The journal
            # carries only bounded, non-secret indexes and measurements.
            if isinstance(value, (Mapping, list, tuple, set, bytes, bytearray)):
                raise RunnerError("LIFECYCLE_FIELD_FORBIDDEN:" + str(key))
            if value is not None and not isinstance(value, (str, int, float, bool)):
                raise RunnerError("LIFECYCLE_FIELD_FORBIDDEN:" + str(key))
        event = {
            "schema": "spec180-yolo-lifecycle-event-v1",
            "caseId": self.case,
            "requestId": self.request_id,
            "attemptId": self.attempt_id,
            "sequence": self._index,
            "timestampUnix": time.time(),
            "milestone": milestone,
            **fields,
        }
        self._path.parent.mkdir(parents=True, exist_ok=True)
        with self._path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, ensure_ascii=False,
                                    sort_keys=True, separators=(",", ":")) + "\n")
        self._seen.add(milestone)
        self._index += 1

    def validate_complete(self) -> None:
        if self._index != len(MILESTONES):
            raise RunnerError("LIFECYCLE_INCOMPLETE")


def _focused_digest(label: str) -> str:
    """Return a deterministic non-secret digest for a focused probe."""
    return "sha256:" + hashlib.sha256(label.encode("utf-8")).hexdigest()


def _new_y_n_subcase_dir(output: Path, subcase: str) -> Path:
    if subcase not in YN_SUBCASES:
        raise RunnerError("Y_N_SUBCASE_UNKNOWN:" + str(subcase))
    root = output / "subcases"
    root.mkdir(parents=True, exist_ok=True)
    target = root / subcase
    if target.exists():
        raise RunnerError("Y_N_SUBCASE_OUTPUT_NOT_FRESH:" + subcase)
    target.mkdir()
    return target


def _write_subcase_result(output: Path, *, subcase: str, status: str,
                          outcome: str, reason: str,
                          child_count: int = 0) -> Mapping[str, Any]:
    """Write one bounded, non-secret result for a fixed Y-N subcase."""
    if subcase not in YN_SUBCASES:
        raise RunnerError("Y_N_SUBCASE_UNKNOWN:" + str(subcase))
    expected = "CONTROL" if subcase == "Y-N-O" else "FAIL_CLOSED"
    result = {
        "schema": "spec180-yolo-subcase-result-v1",
        "caseId": "Y-N",
        "subcaseId": subcase,
        "expected": expected,
        "boundary": YN_SUBCASE_BOUNDARIES[subcase],
        "status": status,
        "outcome": outcome,
        "reason": reason,
        "terminalResponse": subcase == "Y-N-O" and status == "PASS",
        "childCount": int(child_count),
    }
    if _contains_secret(result):
        raise RunnerError("Y_N_SUBCASE_RESULT_SECRET_FIELD")
    path = output / "subcase-result.json"
    path.write_text(json.dumps(result, sort_keys=True, indent=2) + "\n",
                    encoding="utf-8")
    return result


def _focused_role_spec(role: str, index: int):
    from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec

    digest = _focused_digest("spec180:" + role)
    return RoleAssemblySpec(
        role=role, rank=0, layer_begin=0, layer_end=0,
        recipe_digest=digest, artifact_digest=_focused_digest("artifact:" + role),
        backend="onnxruntime-cpu", role_kind="COMPONENT_SET",
        node_indices=(index,),
    )


def _focused_provider_view(provider: str, roles: tuple[str, ...]):
    from ndnsf_distributed_inference.sdk.placement import (
        DeviceTopologyProfile, ExecutionDisposition, ProviderPlanningViewV3,
    )

    return ProviderPlanningViewV3(
        provider=provider,
        offer_digest=_focused_digest("offer:" + provider),
        request_id="/spec180-focused-request",
        attempt=1,
        topology=DeviceTopologyProfile(provider, (), "cpu"),
        resources=(), residency=(), accepted_roles=roles,
        backends=("onnxruntime-cpu",),
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True, queue_depth=0, estimated_wait_ms=0.0,
        rtt_ms=0.0, bandwidth_mbps=0.0, boot_epoch="spec180-boot-1",
        model_digest=_focused_digest("model"),
        graph_digest=_focused_digest("graph"),
    )


def _run_y_n_c_negative() -> None:
    """Drive the real V3 strategy with a closed, infeasible ACK projection."""
    from ndnsf_distributed_inference.planner.presplit_first import (
        PreSplitFirstStrategy,
    )

    # The mutation removes FullModel and Merge from the ACK capability set.
    # Thus atomic-v1 and shared-backbone-two-shard-v1 are both infeasible;
    # this is deliberately a strategy-boundary probe, not a fake terminal.
    providers = (
        _focused_provider_view("/example/provider/FullModel", ("BackboneNeck",)),
        _focused_provider_view("/example/provider/DetectShard0", ("DetectShard0",)),
        _focused_provider_view("/example/provider/DetectShard1", ("DetectShard1",)),
        _focused_provider_view("/example/provider/Merge", ("DetectShard0",)),
    )
    strategy = PreSplitFirstStrategy(at_ms=1)
    for candidate, roles in (
            ("atomic-v1", ("FullModel",)),
            ("shared-backbone-two-shard-v1",
             ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"))):
        try:
            strategy.propose_v3(
                request_id="/spec180-focused-request", attempt=1,
                model_digest=_focused_digest("model"),
                graph_digest=_focused_digest("graph"),
                roles=tuple(_focused_role_spec(role, index)
                            for index, role in enumerate(roles)),
                providers=providers,
                ack_closed_digest=_focused_digest("ack-closed"),
            )
        except ValueError as exc:
            if "no distinct feasible Provider" not in str(exc):
                raise RunnerError("Y_N_C_UNEXPECTED_STRATEGY_REJECTION") from exc
            continue
        raise RunnerError("Y_N_C_CANDIDATE_REMAINED_FEASIBLE:" + candidate)


def _run_y_n_p_negative() -> None:
    """Drive the ACK-aware Trust-Schema/offer verifier with bad provenance."""
    import base64 as _base64
    from types import SimpleNamespace
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from ndnsf_distributed_inference.app_sdk.placement import (
        v3_provider_view_factory,
    )
    from ndnsf_distributed_inference.app_sdk.provider import (
        ProviderOfferTrustVerifier,
    )
    from ndnsf_distributed_inference.sdk.placement import (
        DeviceTopologyProfile, ExecutionDisposition, ProviderOfferV3,
    )

    private = Ed25519PrivateKey.generate()
    public = private.public_key()
    public_raw = public.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_id = "sha256:" + hashlib.sha256(public_raw).hexdigest()
    model_digest = _focused_digest("model")
    graph_digest = _focused_digest("graph")
    now_ms = int(time.time() * 1000)
    provider = "/example/provider/FullModel"
    service = "/AI/YOLO/YOLO26n"
    unsigned = ProviderOfferV3(
        request_id="/spec180-focused-request", attempt=1, service=service,
        provider=provider, model_digest=model_digest,
        graph_digest=graph_digest, status=True,
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True,
        topology=DeviceTopologyProfile(provider, (), "cpu"),
        accepted_roles=("FullModel",), backends=("onnxruntime-cpu",),
        boot_epoch="spec180-boot-1", captured_at_ms=now_ms - 1000,
        expires_at_ms=now_ms + 60000, signer_key_id=key_id,
        signature="pending",
    )
    signature = _base64.b64encode(
        private.sign(unsigned.digest().encode("utf-8"))).decode("ascii")
    offer = ProviderOfferV3(**{**unsigned.__dict__, "signature": signature})
    policy = {
        "schema": "spec180-provider-offer-trust-v1",
        "candidateId": "atomic-v1",
        "candidateDigest": _focused_digest("candidate"),
        "trustSchema": "/example/controller/KEY/offer",
        "entries": [{
            "provider": provider, "service": service,
            "keyLocatorPrefix": provider + "/KEY/",
            "signerKeyId": key_id,
            "certificateName": provider + "/KEY/k1/self/v1",
        }],
    }
    public_pem = public.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    verifier = ProviderOfferTrustVerifier(
        policy, {key_id: public_pem},
        trust_schema_verifier=lambda _ack: True,
        clock_ms=lambda: now_ms,
    )
    ack = SimpleNamespace(
        status=True, provider_name=provider, service_name=service,
        request_id="/spec180-focused-request", attempt=1,
        signer_identity="/example/provider/TAMPERED",
        signer_key_locator=provider + "/KEY/k1",
        validated_wire_digest=_focused_digest("wire"),
        payload=offer.to_bytes(), trust_schema_validated=True,
    )
    try:
        v3_provider_view_factory(verifier)(
            ack, model_digest, now_ms + 30000, graph_digest)
    except ValueError as exc:
        if "signer" not in str(exc).lower():
            raise RunnerError("Y_N_P_UNEXPECTED_TRUST_REJECTION") from exc
        return
    raise RunnerError("Y_N_P_TAMPERED_PROVENANCE_ACCEPTED")


def _run_y_n_r_negative() -> None:
    from ndnsf_distributed_inference.sdk.placement import RoleAssemblySpec

    try:
        RoleAssemblySpec(
            role="BackboneNeck", rank=0, layer_begin=0, layer_end=0,
            recipe_digest=_focused_digest("recipe"),
            artifact_digest=_focused_digest("artifact"),
            backend="onnxruntime-cpu", role_kind="PIPELINE_RANGE",
        )
    except ValueError as exc:
        if "non-empty layer interval" not in str(exc):
            raise RunnerError("Y_N_R_UNEXPECTED_ROLE_REJECTION") from exc
        return
    raise RunnerError("Y_N_R_INVALID_ROLE_KIND_ACCEPTED")


def _focused_request_wire() -> bytes:
    from ndnsf_distributed_inference.adapters import ApplicationInput
    from ndnsf_distributed_inference.app_sdk.placement import (
        AutomaticPlanningCoordinator, InferenceTaskRef, ModelRef, TaskOptions,
    )

    task = InferenceTaskRef(
        task_name="object-detection", adapter_name="yolo26n",
        adapter_descriptor_digest=_focused_digest("adapter"),
        adapter_composition_digest=_focused_digest("composition"),
        task_descriptor_digest=_focused_digest("task"),
    )
    model = ModelRef(
        model_name="YOLO26n", content_digest=_focused_digest("model"),
        semantics_digest=_focused_digest("semantics"),
        source_revision="spec180-focused",
    )
    application_input = ApplicationInput.from_inline(
        task_name=task.task_name,
        input_schema_digest=_focused_digest("input-schema"),
        options_schema_digest=_focused_digest("options-schema"),
        payload=b"focused-input", options=b"{}",
    )
    options = TaskOptions(application_input.options_schema_digest, b"{}")
    return AutomaticPlanningCoordinator._encode_request(
        model, task, application_input, options,
        int(time.time() * 1000) + 60000,
        "/spec180-focused-request", "/AI/YOLO/YOLO26n",
        "invocation:spec180-focused", placement_profile="DI_PLACEMENT_V3",
    )


def _run_y_n_i_negative() -> None:
    from ndnsf_distributed_inference.provider import ProviderRuntimeContext

    class _Probe:
        session_id = "/spec180-focused-request"

    context = ProviderRuntimeContext(
        ndnsf=_Probe(), execution=object(), request=_focused_request_wire(),
        role="DetectShard0", input_ingress_owner=False,
        enforce_dataflow_ownership=True,
    )
    try:
        context.fetch_application_input()
    except PermissionError as exc:
        if str(exc) != "DI_INPUT_FETCH_ROLE_MISMATCH":
            raise RunnerError("Y_N_I_UNEXPECTED_INPUT_REJECTION") from exc
        return
    raise RunnerError("Y_N_I_NON_INGRESS_FETCH_ACCEPTED")


def _run_y_n_e_mutations() -> None:
    """Y-N-E focused probe, spec181 T006 regime: three REAL grant mutations
    must reach the implemented verifier (Python and native, parity-locked)
    and be rejected at the authorization boundary (before assembly).

    A mutation that is accepted — or rejected by any synthetic path — raises
    RunnerError instead of recording a PASS.
    """
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
    from ndnsf_distributed_inference.core.protected_artifacts import (
        GrantRequestV1, grant_from_wire, grant_to_wire,
        verify_and_unwrap_grant)
    from ndnsf_distributed_inference.security.artifact_policy_authority import (
        ArtifactPolicyAuthority)
    from ndnsf_distributed_inference.security.grant_mutations import (
        mutate_expired, mutate_forged_authority)

    def _seed(label: str) -> bytes:
        return hashlib.sha256(
            ("spec181-y-n-e-v1:" + label).encode("utf-8")).digest()

    authority_key = ed25519.Ed25519PrivateKey.from_private_bytes(
        _seed("authority"))
    recipient_key = ed25519.Ed25519PrivateKey.from_private_bytes(
        _seed("recipient"))
    wrong_recipient_key = ed25519.Ed25519PrivateKey.from_private_bytes(
        _seed("wrong-recipient"))
    requester_key = ed25519.Ed25519PrivateKey.from_private_bytes(
        _seed("requester"))
    evil_key = ed25519.Ed25519PrivateKey.from_private_bytes(
        _seed("evil-authority"))
    content_key = _seed("content-key")[:32]
    epoch = "spec180-yolo-protected-v1"
    model_manifest = "sha256:" + "11" * 32
    plan_core = "sha256:" + "cd" * 32
    now_ms = int(time.time() * 1000)
    authority = ArtifactPolicyAuthority(
        "/authority/artifact-policy", authority_key,
        protection_epoch=epoch,
        allowed_model_manifests=frozenset({model_manifest}),
    )

    def issue(recipient):
        request = GrantRequestV1(
            provider_identity="/provider/p0", request_id="req-y-n-e-1",
            attempt=1, plan_core_digest=plan_core,
            grant_view_digest="sha256:" + "55" * 32,
            model_manifest_digest=model_manifest, protection_epoch=epoch,
            requester_identity="/user/u0", issued_at_ms=now_ms,
        ).sign(requester_key)
        return authority.issue(
            request,
            requester_public_key=requester_key.public_key(),
            recipient_public_key=recipient.public_key(),
            content_key=content_key, key_id="key-y-n-e-1",
            expires_at_ms=now_ms + 60_000, now_ms=now_ms)

    mutations = {
        "EXPIRED": mutate_expired(
            issue(recipient_key), authority_key,
            expires_at_ms=now_ms - 1),
        "WRONG_RECIPIENT": issue(wrong_recipient_key),
        "FORGED_AUTHORITY": mutate_forged_authority(
            issue(recipient_key), evil_key),
    }

    def python_verifier(grant):
        verify_and_unwrap_grant(
            grant,
            authority_public_key=authority_key.public_key(),
            recipient_private_key=recipient_key,
            expected_provider_identity="/provider/p0",
            expected_request_id="req-y-n-e-1", expected_attempt=1,
            expected_plan_core_digest=plan_core,
            expected_model_manifest_digest=model_manifest,
            expected_protection_epoch=epoch,
            now_ms=now_ms + 1000)

    def native_verifier(grant):
        from ndnsf import _ndnsf
        result = _ndnsf.verify_and_unwrap_native_grant(
            grant_to_wire(grant).decode("utf-8"),
            authority_key.public_key().public_bytes(
                serialization.Encoding.Raw,
                serialization.PublicFormat.Raw).hex(),
            recipient_key.private_bytes(
                serialization.Encoding.Raw,
                serialization.PrivateFormat.Raw,
                serialization.NoEncryption()).hex(),
            "/provider/p0", "req-y-n-e-1", 1, plan_core,
            model_manifest, epoch, now_ms + 1000)
        if bool(result["verified"]):
            raise ValueError("native verifier accepted the mutation")
        # A native rejection is the expected outcome: surface the
        # registered reason through the same ValueError channel so the
        # reason-family check applies to both verifiers identically.
        raise ValueError(str(result["reason"]))

    for name, mutation in mutations.items():
        for label, verifier in (("python", python_verifier),
                                ("native", native_verifier)):
            try:
                verifier(mutation)
            except ValueError as exc:
                reason = str(exc)
                if ("grant" not in reason.lower()
                        and "binding" not in reason.lower()
                        and "envelope" not in reason.lower()
                        and "expired" not in reason.lower()
                        and "signature" not in reason.lower()
                        and "authentication" not in reason.lower()
                        and "digest" not in reason.lower()):
                    raise RunnerError(
                        f"Y_N_E_UNREGISTERED_REJECTION:{name}:{label}"
                    ) from exc
                continue
            raise RunnerError(f"Y_N_E_MUTATION_ACCEPTED:{name}:{label}")


def _run_y_n_l_negative(output: Path) -> None:
    journal = LifecycleJournal(output, "Y-N-L", require_protocol_binding=False)
    try:
        journal.append("INPUT_REFERENCE_PUBLISHED", payload="redacted-test")
    except RunnerError as exc:
        if "FIELD_FORBIDDEN:payload" not in str(exc):
            raise RunnerError("Y_N_L_UNEXPECTED_REDACTION_REJECTION") from exc
        return
    raise RunnerError("Y_N_L_PLAINTEXT_FIELD_ACCEPTED")


def _run_focused_y_n_negative(subcase: str, output: Path,
                              inputs: Mapping[str, Any]) -> Mapping[str, Any]:
    if subcase == "Y-N-E":
        raise RunnerError("Y_N_E_PRODUCTION_VERIFIER_REQUIRED")
    del inputs  # The focused probes use only fixed, non-secret contract fixtures.
    target = _new_y_n_subcase_dir(output, subcase)
    probes = {
        "Y-N-C": _run_y_n_c_negative,
        "Y-N-P": _run_y_n_p_negative,
        "Y-N-R": _run_y_n_r_negative,
        "Y-N-I": _run_y_n_i_negative,
        "Y-N-L": lambda: _run_y_n_l_negative(target),
    }
    try:
        probes[subcase]()
    except RunnerError:
        raise
    except Exception as exc:
        raise RunnerError("Y_N_NEGATIVE_PROBE_FAILED:" + subcase) from exc
    return _write_subcase_result(
        target, subcase=subcase, status="PASS", outcome="FAIL_CLOSED",
        reason=YN_NEGATIVE_REASONS[subcase],
    )


def _validate_negative_marker(line, spec, subcase, user_spec, user_log):
    if subcase == "Y-N-E":
        raise RunnerError("Y_N_E_REQUIRES_PROVIDER_VERIFIER_RECORD")
    if (subcase == "Y-N-I" and spec.startup_phase != "providers"
            or subcase != "Y-N-I" and spec.name != "user"):
        raise RunnerError("Y_N_NEGATIVE_OWNER_MISMATCH:" + subcase)
    try:
        pairs = [token.split("=", 1) for token in line.split()[1:]]
        fields = dict(pairs)
        if len(fields) != len(pairs):
            raise ValueError("duplicate fields")
    except ValueError:
        raise RunnerError("Y_N_NEGATIVE_MARKER_INVALID:" + subcase) from None
    # Y-N-E uses its separate Provider-verifier collector above.
    expected = {"status": "PASS", "subcase": subcase,
                "boundary": YN_SUBCASE_BOUNDARIES[subcase],
                "reason": YN_NEGATIVE_REASONS[subcase]}
    allowed_fields = {*expected, "requestId", "attemptId", "observedPhase"}
    if subcase == "Y-N-I":
        allowed_fields.update({"provider", "planDigest", "errorCode"})
    if set(fields) != allowed_fields:
        raise RunnerError("Y_N_NEGATIVE_IDENTITY_FIELDS_INVALID:" + subcase)
    if any(fields.get(key) != value for key, value in expected.items()):
        raise RunnerError("Y_N_NEGATIVE_MARKER_INVALID:" + subcase)
    command = shlex.split(user_spec.command)
    try:
        request_id = command[command.index("--request-id") + 1]
    except (ValueError, IndexError):
        raise RunnerError("Y_N_NEGATIVE_REQUEST_IDENTITY_MISSING") from None
    if (fields.get("requestId") != request_id
            or fields.get("attemptId") != "attempt-1"):
        raise RunnerError("Y_N_NEGATIVE_IDENTITY_MISMATCH:" + subcase)
    expected_phase = {"Y-N-C": "GRAPH_READY", "Y-N-P": "GRAPH_READY",
                      "Y-N-R": "PLACEMENT_DECISION",
                      "Y-N-I": "PROVIDER_EXECUTION_STARTED",
                      "Y-N-L": "INPUT_REFERENCE_PUBLISHED"}[subcase]
    try:
        events = [json.loads(row) for row in
                  (user_log.parent / "lifecycle.jsonl").read_text().splitlines()]
    except (OSError, ValueError):
        raise RunnerError("Y_N_NEGATIVE_LIFECYCLE_MISSING:" + subcase) from None
    observed_phase = fields.get("observedPhase", "")
    allowed_phase = observed_phase == expected_phase
    if (not events or any(not isinstance(event, dict) for event in events)
            or not allowed_phase
            or [event.get("milestone") for event in events]
            != list(MILESTONES[:MILESTONES.index(observed_phase) + 1])
            or any(event.get("requestId") != request_id
                   or event.get("attemptId") != "attempt-1"
                   or event.get("caseId") != subcase
                   or event.get("sequence") != index
                   for index, event in enumerate(events))):
        raise RunnerError("Y_N_NEGATIVE_LIFECYCLE_MISMATCH:" + subcase)
    if subcase == "Y-N-I":
        provider_command = shlex.split(spec.command)
        try:
            provider = provider_command[provider_command.index("--provider") + 1]
        except (ValueError, IndexError):
            raise RunnerError("Y_N_NEGATIVE_OWNER_MISSING:" + subcase) from None
        plan = next(event for event in events if event["milestone"] == "PLAN_SEALED")
        if (fields.get("provider") != provider
                or not _DIGEST_RE.fullmatch(fields.get("planDigest", ""))
                or fields["planDigest"] != plan.get("planDigest")
                or fields.get("errorCode") != "DI_INPUT_FETCH_ROLE_MISMATCH"):
            raise RunnerError("Y_N_NEGATIVE_PROVIDER_BINDING_MISMATCH:" + subcase)


def _validate_grant_rejection(record, publication, *, spec, user_spec,
                              user_log, variant):
    """Bind an actual verifier decision to the selected published grant."""
    record_fields = {"status", "boundary", "provider", "requestId", "attemptId",
                     "planCoreDigest", "planDigest", "grantDigest", "reason"}
    publication_fields = {"variant", "provider", "requestId", "attemptId",
                          "planCoreDigest", "grantDigest"}
    if (not isinstance(record, dict) or set(record) != record_fields
            or not isinstance(publication, dict) or set(publication) != publication_fields
            or variant not in YN_GRANT_REJECTIONS
            or publication.get("variant") != variant
            or record.get("status") != "REJECTED"
            or record.get("boundary") != "BEFORE_ASSEMBLY"
            or record.get("reason") != YN_GRANT_REJECTIONS[variant]):
        raise RunnerError("Y_N_E_VERIFIER_REASON_INVALID")
    if getattr(spec, "startup_phase", "") != "providers":
        raise RunnerError("Y_N_E_VERIFIER_OWNER_INVALID")
    try:
        command = shlex.split(user_spec.command)
        request_id = command[command.index("--request-id") + 1]
        command = shlex.split(spec.command)
        provider = command[command.index("--provider") + 1]
    except (ValueError, IndexError):
        raise RunnerError("Y_N_E_PROCESS_IDENTITY_MISSING") from None
    if (record["requestId"] != request_id or record["provider"] != provider
            or record["attemptId"] != "attempt-1"
            or any(record[key] != publication[key] for key in
                   ("provider", "requestId", "attemptId", "planCoreDigest", "grantDigest"))
            or any(not isinstance(record[key], str) or not _DIGEST_RE.fullmatch(record[key])
                   for key in ("planCoreDigest", "planDigest", "grantDigest"))):
        raise RunnerError("Y_N_E_GRANT_BINDING_MISMATCH")
    try:
        events = [json.loads(row) for row in
                  (user_log.parent / "lifecycle.jsonl").read_text().splitlines()]
    except (OSError, ValueError):
        raise RunnerError("Y_N_E_LIFECYCLE_MISSING") from None
    expected = list(MILESTONES[:MILESTONES.index("PROVIDER_EXECUTION_STARTED") + 1])
    if (any(not isinstance(event, dict) for event in events)
            or [event.get("milestone") for event in events] != expected
            or any(event.get("requestId") != request_id
                   or event.get("attemptId") != "attempt-1"
                   or event.get("caseId") != "Y-N-E"
                   or event.get("sequence") != index
                   for index, event in enumerate(events))
            or next(event for event in events if event["milestone"] == "PLAN_SEALED")
               .get("planDigest") != record["planDigest"]):
        raise RunnerError("Y_N_E_LIFECYCLE_BINDING_MISMATCH")
    return ("SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=Y-N-E"
            " boundary=PROVIDER_GRANT_VERIFICATION reason=DI_PROTECTED_GRANT_REJECTED"
            f" requestId={request_id} attemptId=attempt-1"
            " observedPhase=PROVIDER_EXECUTION_STARTED"
            f" provider={provider} planDigest={record['planDigest']}"
            f" grantDigest={record['grantDigest']} variant={variant}")


def _wait_for_grant_rejection(started, timeout_s):
    variant = os.environ.get("SPEC181_GRANT_MUTATION", "")
    if variant not in YN_GRANT_REJECTIONS:
        raise RunnerError("GRANT_MUTATION_INVALID")
    users = [(spec, path) for spec, _proc, path in started if spec.name == "user"]
    if len(users) != 1:
        raise RunnerError("Y_N_E_USER_IDENTITY_MISSING")
    user_spec, user_log = users[0]
    deadline = time.monotonic() + timeout_s

    def records(path, prefix):
        if not path.exists():
            return []
        try:
            return [json.loads(line[len(prefix):]) for line in
                    path.read_text().splitlines() if line.startswith(prefix)]
        except (ValueError, UnicodeError):
            raise RunnerError("Y_N_E_VERIFIER_RECORD_INVALID") from None

    while True:
        publications = records(user_log, "SPEC181_GRANT_MUTATION_PUBLISHED ")
        if len(publications) > 1:
            raise RunnerError("Y_N_E_MULTIPLE_MUTATED_GRANTS")
        accepted = None
        for spec, proc, path in started:
            text = path.read_text(errors="replace") if path.exists() else ""
            if "YOLO_ACK_DRIVEN_RESULT status=true" in text or "GRANT_MUTATION_WAS_ACCEPTED" in text:
                raise RunnerError("Y_N_E_MUTATION_ACCEPTED")
            if "SPEC180_YN_NEGATIVE_RESULT " in text:
                raise RunnerError("Y_N_E_REQUIRES_PROVIDER_VERIFIER_RECORD")
            if publications:
                for record in records(path, "NDNSF_DI_GRANT_VERIFICATION "):
                    if not isinstance(record, dict):
                        raise RunnerError("Y_N_E_VERIFIER_RECORD_INVALID")
                    if (record.get("status") == "VERIFIED"
                            and record.get("grantDigest") != publications[0].get("grantDigest")):
                        continue
                    # The Provider can finish before the requester appends its
                    # execution milestone. Wait for that independent binding.
                    lifecycle = user_log.parent / "lifecycle.jsonl"
                    if (not lifecycle.exists()
                            or '"PROVIDER_EXECUTION_STARTED"' not in lifecycle.read_text()):
                        continue
                    marker = _validate_grant_rejection(
                        record, publications[0], spec=spec, user_spec=user_spec,
                        user_log=user_log, variant=variant)
                    if accepted is not None:
                        raise RunnerError("Y_N_E_DUPLICATE_VERIFIER_REJECTION")
                    accepted = (marker, record, publications[0])
            code = proc.poll()
            if code is not None and not (spec.name == "user" and code == YN_NEGATIVE_PASS_EXIT):
                raise RunnerError("Y_N_NEGATIVE_CHILD_FAILURE:" + spec.name)
        if accepted is not None:
            marker, record, publication = accepted
            (user_log.parent / "grant-rejection-evidence.json").write_text(json.dumps({
                "status": "OBSERVED", "verifier": record, "publication": publication,
            }, sort_keys=True, indent=2) + "\n")
            return marker
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RunnerError("Y_N_E_PROVIDER_REJECTION_NOT_PROVEN")
        time.sleep(min(0.2, remaining))


def _wait_for_negative_result(
        started: tuple[tuple[CaseProcessSpec, object, Path], ...],
        subcase: str, timeout_s: float) -> str:
    """Wait for a bounded negative marker from the real child path.

    Negative Y-N cases intentionally do not publish the normal user-ready
    marker.  The marker may be emitted by the User (request/plan/redaction
    boundaries) or by the Provider (input-ingress ownership), so inspect the
    complete child set while retaining the same process barrier and teardown
    path as control cases.
    """
    if subcase not in YN_SUBCASES[1:]:
        raise RunnerError("Y_N_NEGATIVE_SUBCASE_INVALID:" + subcase)
    if subcase == "Y-N-E":
        return _wait_for_grant_rejection(started, timeout_s)
    users = [(spec, path) for spec, _proc, path in started if spec.name == "user"]
    if len(users) != 1:
        raise RunnerError("Y_N_NEGATIVE_USER_IDENTITY_MISSING")
    user_spec, user_log = users[0]
    deadline = time.monotonic() + float(timeout_s)
    while True:
        all_exited = True
        accepted = []
        for spec, proc, log_path in started:
            text = log_path.read_text(errors="replace") if log_path.exists() else ""
            if "YOLO_ACK_DRIVEN_RESULT status=true" in text:
                raise RunnerError("Y_N_NEGATIVE_TERMINAL_SUCCESS:" + subcase)
            # Python children print the marker as a raw line, while native
            # RuntimeEvidence routes it through the logger and prefixes a
            # timestamp/level.  Keep the marker's validated field grammar,
            # but strip only the logger prefix before parsing it.
            negative_lines = []
            for line in text.splitlines():
                marker = line.find("SPEC180_YN_NEGATIVE_RESULT ")
                if marker >= 0:
                    negative_lines.append(line[marker:])
            for line in negative_lines:
                _validate_negative_marker(line, spec, subcase, user_spec, user_log)
                accepted.append(line)
            poll = getattr(proc, "poll", None)
            return_code = poll() if callable(poll) else None
            if return_code is None:
                all_exited = False
            elif not (spec.name == "user" and
                      return_code == YN_NEGATIVE_PASS_EXIT):
                raise RunnerError("Y_N_NEGATIVE_CHILD_FAILURE:" + spec.name)
        # Inspect all children even if the first log already had a marker.
        if accepted:
            return accepted[-1]
        if all_exited:
            raise RunnerError("Y_N_NEGATIVE_MARKER_MISSING:" + subcase)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RunnerError("Y_N_NEGATIVE_TIMEOUT:" + subcase)
        time.sleep(min(0.2, remaining))


def _close_case_children(runtime, started, *, expected_user_exit, timeout_s=5.0):
    """Require the User's own exit, then bounded, successful sibling cleanup."""
    users = [proc for spec, proc, _path in started if spec.name == "user"]
    if len(users) != 1:
        raise RunnerError("CASE_TERMINAL_USER_SET_INVALID")
    deadline = time.monotonic() + timeout_s
    while True:
        for spec, proc, _path in started:
            code = proc.poll()
            if code is not None and (spec.name != "user" or code != expected_user_exit):
                raise RunnerError("CASE_TERMINAL_CHILD_FAILURE:" + spec.name + ":" + str(code))
        if users[0].poll() == expected_user_exit:
            break
        if time.monotonic() >= deadline:
            raise RunnerError("CASE_TERMINAL_USER_DID_NOT_EXIT")
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
    before = {spec.name: proc.poll() for spec, proc, _path in started}
    runtime.stop()
    records = []
    for spec, proc, _path in started:
        code = proc.poll()
        allowed = ({expected_user_exit} if spec.name == "user"
                   else {0, -signal.SIGINT, 128 + signal.SIGINT})
        if code not in allowed:
            raise RunnerError("CASE_TERMINAL_CLEANUP_FAILURE:" + spec.name + ":" + str(code))
        records.append({"name": spec.name, "pid": getattr(proc, "pid", None),
                        "exitStatus": code,
                        "terminationRequested": before[spec.name] is None})
    return records


def _run_live_case_once(case: str, output: Path, inputs: Mapping[str, Any], *,
                        subcase: str = "") -> int:
    """Run one real, barriered MiniNDN case through the maintained API.

    This is intentionally a small vertical slice: one candidate-bound
    publication batch, one Controller/Repository/Provider startup, one User
    request, and one terminal result.  Y-B/Y-N reuse the same driver after
    Y-A is proven; the function never calls the legacy deployment-first main.
    """
    runtime_inputs = dict(inputs)
    if subcase:
        runtime_inputs["subcase"] = subcase
        runtime_inputs["lifecycle_case"] = subcase
    # spec181 T008: declare the protected epoch on the same channel the
    # process specs read (the Y-B grant round trip).
    requested_epoch = str(
        os.environ.get(PROTECTION_EPOCH_ENV, "") or "").strip()
    if subcase == "Y-N-E":
        if os.environ.get("SPEC181_GRANT_MUTATION", "") not in YN_GRANT_REJECTIONS:
            raise RunnerError("GRANT_MUTATION_INVALID")
        if not requested_epoch or requested_epoch == PLAINTEXT_EPOCH:
            raise RunnerError("GRANT_MUTATION_REQUIRES_PROTECTED_EPOCH")
    if (case == "Y-B" or subcase == "Y-N-E") and requested_epoch and requested_epoch != PLAINTEXT_EPOCH:
        runtime_inputs["protection_epoch"] = requested_epoch
    # MiniNDN's systemd owner is root so it can create network namespaces.
    # RuntimeJournal intentionally rejects a key owned by another euid; stage
    # one root-owned copy for exact-SIF children while retaining the original
    # operator-owned key as the validated input and digest source.
    if sif_runtime_enabled() and os.geteuid() == 0:
        runtime_inputs["envelope_key_file"] = _stage_sif_owner_key(
            Path(runtime_inputs["envelope_key_file"]), output)
    binding = CaseRuntimeBinding.from_inputs(case, output, runtime_inputs)
    publication = build_runtime_publication_file(binding, runtime_inputs)
    runtime_inputs["runtime_publication_file"] = publication
    runtime = MiniNdnCaseRuntime(binding, runtime_inputs)
    processes: list[tuple[object, object, Path]] = []
    # Keep the child environment explicit and reproducible.  In particular,
    # do not inherit a caller's PYTHONPATH ordering for an ACK-driven case.
    py_dir = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2"
    env = _child_process_environment(os.environ)
    # The matrix-level epoch selects protected Y-N-E only. Use the same
    # per-case epoch as publication and Provider process specs for every child.
    env[PROTECTION_EPOCH_ENV] = str(
        runtime_inputs.get("protection_epoch", PLAINTEXT_EPOCH))
    # spec181 T008: Y-B runs the protected-epoch grant round trip.  The
    # requester-side User builds the in-process authority seam from these
    # inputs; every Provider resolves its own recipient key from the configured
    # recipient map (the offer-key map is the backward-compatible default).
    # Plaintext Y-A keeps the default epoch.
    if case == "Y-B" or subcase == "Y-N-E":
        requested_epoch = str(env.get(PROTECTION_EPOCH_ENV, "") or "").strip()
        if requested_epoch and requested_epoch != PLAINTEXT_EPOCH:
            env[PROTECTION_EPOCH_ENV] = requested_epoch
            env["SPEC181_REQUESTER_PRIVATE_KEY"] = str(
                env.get("SPEC181_REQUESTER_PRIVATE_KEY") or
                env.get("NDNSF_DI_ENVELOPE_KEY_FILE", ""))
            env["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"] = str(
                env.get("SPEC181_PROVIDER_RECIPIENT_KEY_MAP") or
                env.get("SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP", ""))
            env["SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"] = str(
                env.get("SPEC181_GRANT_AUTHORITY_PUBLIC_KEY") or
                ROOT / "specs/180-ack-driven-cross-model-qualification"
                / "contracts/artifact-policy-authority.pub")
            # Resolve the selected external config before MiniNDN changes
            # child HOME/cwd. An explicit root must never fall back to another
            # authority key under the operator's default directory.
            config_root = (env.get("NDNSF_SPEC180_CONFIG_ROOT") or
                           str(Path.home() / ".config" / "ndnsf" / "spec180"))
            env["NDNSF_SPEC180_CONFIG_ROOT"] = str(Path(config_root).expanduser().resolve())
            if not (Path(env["NDNSF_SPEC180_CONFIG_ROOT"])
                    / "artifact-policy-authority.key").is_file():
                raise RunnerError(
                    "PROTECTED_EPOCH_AUTHORITY_PRIVATE_KEY_MISSING")
            # configure_routing() installs the requester's identity route
            # before startup; exact grants use that route without a hint.
            if not env["SPEC181_REQUESTER_PRIVATE_KEY"]:
                raise RunnerError("PROTECTED_EPOCH_REQUESTER_KEY_MISSING")
            if not env["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"]:
                raise RunnerError("PROTECTED_EPOCH_RECIPIENT_KEY_MAP_MISSING")
            if not Path(env["SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"]).is_file():
                raise RunnerError("PROTECTED_EPOCH_AUTHORITY_PUBLIC_KEY_MISSING")
            # Native Providers consume the same pinned registry and recipient
            # map through their protected factory; no process substitution.
    # Y-N-O is the live control permutation, not a mutation.  Keep the
    # subcase in the lifecycle identity while leaving the User on its normal
    # control path; the negative User hook accepts only Y-N-C/P/R/I/E/L.
    if subcase in YN_SUBCASES[1:]:
        env["SPEC180_YN_MUTATION"] = subcase
    else:
        env.pop("SPEC180_YN_MUTATION", None)
    env["PYTHONPATH"] = ":".join(filter(None, (
        str(ROOT / "NDNSF-DistributedInference"),
        str(ROOT / "NDNSF-DistributedRepo/pythonWrapper"),
        str(ROOT / "pythonWrapper"),
        str(py_dir),
        # The local qualification host installs onnxruntime and the Python
        # bindings in the interpreter's user site.  HOME is intentionally
        # node-scoped for NDN PIB/TPM isolation, so retain this dependency
        # path explicitly instead of relying on Python's user-site discovery.
        str(site.getusersitepackages())
        if Path(site.getusersitepackages()).is_dir() else "",
        env.get("PYTHONPATH", ""),
    )))
    # APPDeployment/APPClient use a durable journal even for this bounded
    # qualification.  Give every process the same case-scoped persistent
    # root; requester namespaces keep client records distinct, while the
    # controller/repository construction remains read-compatible.
    env["NDNSF_DI_STATE_ROOT"] = str(
        runtime_inputs.get("state_root") or os.environ.get("NDNSF_DI_STATE_ROOT", ""))
    if sif_runtime_enabled() and os.geteuid() == 0:
        env["NDNSF_DI_ENVELOPE_KEY_FILE"] = str(
            runtime_inputs["envelope_key_file"])
    # The local MiniNDN supervisor is root so it can create network
    # namespaces, while RuntimeJournal inside an exact-SIF application binds
    # its root to the process UID. Keep the operator-owned outer state root
    # for preflight, and give privileged SIF children a fresh root-owned
    # journal directory inside this case's evidence tree. Tiger/Slurm runs do
    # not enter this branch because their application UID is already the
    # operator UID.
    if sif_runtime_enabled() and os.geteuid() == 0:
        child_state_root = binding.output / ".sif-runtime-state"
        if child_state_root.exists() or child_state_root.is_symlink():
            raise RunnerError("SIF_RUNTIME_STATE_ROOT_REUSED")
        child_state_root.mkdir(mode=0o700, parents=False, exist_ok=False)
        os.chown(child_state_root, os.geteuid(), os.getegid())
        env["NDNSF_DI_STATE_ROOT"] = str(child_state_root)
    env.setdefault("NDNSF_HANDLER_THREADS", "1")
    env.setdefault("NDNSF_ACK_THREADS", "1")
    cleanup_done = False
    try:
        # Validate the complete child vector, including private offer-key
        # bindings and the Y-A local model path, before creating MiniNDN/NFD.
        # This keeps missing candidate material a zero-side-effect failure.
        runtime.process_specs()
        # A matching soname is insufficient: the previous developer run
        # loaded libndn-cxx from .local-boost171 for Python/NDN-SVS while NFD
        # loaded a different /usr/local build, producing socket EOF before
        # any request.  Reject that split runtime before MiniNDN side effects.
        _validate_native_library_closure()
        _validate_local_native_build(env)
        ndn = runtime.start_network()
        runtime.configure_routing(ndn)
        # The provision stage already installs the issuer-signed PIB/TPM for
        # every role under its private HOME.  Regenerating a root and child
        # certificates here (the legacy MiniNDN helper) would silently replace
        # those files with a second trust domain, so the exact-SIF Validator
        # follows a chain that is absent from the prepared /config/root.cert.
        # Keep the legacy generator for host-source compatibility runs only.
        if not sif_runtime_enabled():
            runtime.initialize_keychains(ndn)
        phase_started: list[tuple[CaseProcessSpec, object, Path]] = []
        started = runtime.start_processes(ndn, env, processes, phase="control")
        phase_started.extend(started)
        runtime.wait_for_ready(started, 90.0)
        started = runtime.start_processes(ndn, env, processes, phase="providers")
        phase_started.extend(started)
        runtime.wait_for_ready(started, 90.0)
        receipt = _wait_for_runtime_receipt(binding.output, publication, 30.0)
        runtime.mark_catalogue_published(
            data_name=str(receipt["catalogueDataName"]),
            signer=str(receipt["catalogueSigner"]),
            data_digest=str(receipt["cataloguePayloadDigest"]),
        )
        started = runtime.start_processes(ndn, env, processes, phase="user")
        phase_started.extend(started)
        if subcase in YN_SUBCASES[1:]:
            negative_marker = _wait_for_negative_result(tuple(phase_started), subcase, 120.0)
            # A negative marker is only an admission decision.  Close the
            # same child set before writing the subcase result, then verify
            # that every process has actually terminated.  This prevents a
            # provider that emitted the marker while continuing to serve from
            # being mistaken for a clean fail-closed case.
            marker_fields = dict(
                token.split("=", 1) for token in negative_marker.split()[1:])
            children = _close_case_children(
                runtime, phase_started,
                expected_user_exit=YN_NEGATIVE_PASS_EXIT)
            cleanup_done = True
            negative_evidence = {
                "schema": "spec180-negative-evidence-v1",
                **marker_fields,
                "lifecycleSha256": digest_file(binding.output / "lifecycle.jsonl"),
                "children": children,
            }
            (binding.output / "negative-evidence.json").write_text(
                json.dumps(negative_evidence, sort_keys=True, indent=2) + "\n",
                encoding="utf-8")
            _write_subcase_result(
                binding.output, subcase=subcase, status="PASS",
                outcome="FAIL_CLOSED", reason=YN_NEGATIVE_REASONS[subcase],
                child_count=len(processes))
            print("SPEC180_SUBCASE_RESULT status=PASS subcase=" + subcase,
                  flush=True)
            return 0
        runtime.wait_for_ready(started, 120.0)
        user_log = started[0][2]
        text = user_log.read_text(errors="replace") if user_log.exists() else ""
        result_lines = [line for line in text.splitlines()
                        if line.startswith("YOLO_ACK_DRIVEN_RESULT ")]
        if not result_lines or "status=true" not in result_lines[-1]:
            raise RunnerError("CASE_RUNTIME_TERMINAL_RESPONSE_INVALID")
        children = _close_case_children(runtime, phase_started, expected_user_exit=0)
        cleanup_done = True
        (binding.output / "child-exits.json").write_text(
            json.dumps(children, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        (binding.output / "terminal-result.txt").write_text(
            result_lines[-1] + "\n", encoding="utf-8")
        (binding.output / "subcase-result.json").write_text(
            json.dumps({
                "schema": "spec180-yolo-subcase-result-v1",
                "caseId": case, "subcaseId": subcase or case,
                "expected": "CONTROL", "boundary": "TERMINAL_RESPONSE",
                "status": "PASS", "outcome": "CONTROL",
                "reason": "TERMINAL_RESPONSE_VERIFIED",
                "terminalResponse": True, "childCount": len(processes),
            }, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        if subcase:
            print("SPEC180_SUBCASE_RESULT status=PASS subcase=" + subcase,
                  flush=True)
        else:
            print("SPEC180_CASE_RESULT status=PASS case=" + case, flush=True)
    finally:
        primary_failure = sys.exc_info()[0]
        try:
            if not cleanup_done:
                runtime.stop()
        except RunnerError as cleanup_exc:
            # Preserve the original request failure; the teardown error is
            # written to the case directory for the audit boundary.
            (binding.output / "cleanup-error.txt").write_text(
                str(cleanup_exc) + "\n", encoding="utf-8")
            if primary_failure is None:
                raise
    return 0


def _run_y_n_e_variants(output: Path, inputs: Mapping[str, Any]) -> None:
    """Require all three independent production variants; never retry one."""
    previous = os.environ.get("SPEC181_GRANT_MUTATION")
    results = []
    try:
        for variant in YN_GRANT_REJECTIONS:
            target = output / variant
            target.mkdir()  # Exclusive creation preserves every earlier run.
            os.environ["SPEC181_GRANT_MUTATION"] = variant
            if _run_live_case_once("Y-N", target, inputs, subcase="Y-N-E") != 0:
                raise RunnerError("Y_N_E_VARIANT_FAILED:" + variant)
            result = json.loads((target / "subcase-result.json").read_text())
            evidence = json.loads((target / "negative-evidence.json").read_text())
            if (result.get("status") != "PASS" or result.get("outcome") != "FAIL_CLOSED"
                    or result.get("subcaseId") != "Y-N-E"
                    or result.get("boundary") != YN_SUBCASE_BOUNDARIES["Y-N-E"]
                    or evidence.get("variant") != variant
                    or evidence.get("status") != "PASS"
                    or evidence.get("reason") != YN_NEGATIVE_REASONS["Y-N-E"]):
                raise RunnerError("Y_N_E_VARIANT_NOT_PROVEN:" + variant)
            results.append({"variant": variant, "result": result,
                            "evidenceSha256": digest_file(target / "negative-evidence.json")})
    finally:
        if previous is None:
            os.environ.pop("SPEC181_GRANT_MUTATION", None)
        else:
            os.environ["SPEC181_GRANT_MUTATION"] = previous
    (output / "grant-mutation-matrix.json").write_text(json.dumps({
        "schema": "spec181-grant-mutation-matrix-v1", "status": "PASS",
        "variants": results}, sort_keys=True, indent=2) + "\n")
    _write_subcase_result(output, subcase="Y-N-E", status="PASS", outcome="FAIL_CLOSED",
                          reason=YN_NEGATIVE_REASONS["Y-N-E"],
                          child_count=sum(row["result"]["childCount"] for row in results))


def _run_y_n_matrix(output: Path, inputs: Mapping[str, Any]) -> int:
    """Run one ordered matrix; preserve and stop at the first failed subcase."""
    results: list[Mapping[str, Any]] = []
    failures: list[str] = []
    control = _new_y_n_subcase_dir(output, "Y-N-O")
    try:
        _run_live_case_once("Y-N", control, inputs, subcase="Y-N-O")
        result_path = control / "subcase-result.json"
        if not result_path.is_file():
            raise RunnerError("Y_N_O_RESULT_MISSING")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result.get("status") != "PASS" or result.get("outcome") != "CONTROL":
            raise RunnerError("Y_N_O_CONTROL_NOT_PROVEN")
        results.append(result)
    except (RunnerError, OSError, UnicodeError, json.JSONDecodeError):
        failures.append("Y-N-O")
        if not (control / "subcase-result.json").exists():
            _write_subcase_result(
                control, subcase="Y-N-O", status="UNQUALIFIED",
                outcome="CONTROL_NOT_PROVEN", reason="CONTROL_NOT_PROVEN")
        raise RunnerError("Y_N_MATRIX_INCOMPLETE:Y-N-O")

    for subcase in YN_SUBCASES[1:]:
        target = _new_y_n_subcase_dir(output, subcase)
        try:
            if subcase == "Y-N-E":
                _run_y_n_e_variants(target, inputs)
            else:
                _run_live_case_once("Y-N", target, inputs, subcase=subcase)
            result_path = target / "subcase-result.json"
            if not result_path.is_file():
                raise RunnerError("Y_N_NEGATIVE_RESULT_MISSING:" + subcase)
            result = json.loads(result_path.read_text(encoding="utf-8"))
            if (result.get("status") != "PASS"
                    or result.get("outcome") != "FAIL_CLOSED"
                    or result.get("boundary") != YN_SUBCASE_BOUNDARIES[subcase]):
                raise RunnerError("Y_N_NEGATIVE_NOT_PROVEN:" + subcase)
            results.append(result)
        except (RunnerError, OSError, UnicodeError, json.JSONDecodeError):
            failures.append(subcase)
            if not (target / "subcase-result.json").exists():
                _write_subcase_result(
                    target, subcase=subcase, status="UNQUALIFIED",
                    outcome="FAIL_CLOSED_NOT_PROVEN",
                    reason="FAIL_CLOSED_NOT_PROVEN")
            raise RunnerError("Y_N_MATRIX_INCOMPLETE:" + subcase)

    # PASS rows must cover every subcase (spec181 T006: Y-N-E records a
    # registered PASS after the real mutations were verifier-rejected).
    if failures or len(results) != len(YN_SUBCASES):
        raise RunnerError("Y_N_MATRIX_INCOMPLETE:" + ",".join(failures))
    matrix = {
        "schema": "spec180-yolo-yn-matrix-result-v1",
        "caseId": "Y-N",
        "subcases": [
            {"id": item["subcaseId"], "status": item["status"],
             "outcome": item["outcome"], "boundary": item["boundary"]}
            for item in results
        ],
        "aggregate": "PASS",
    }
    if _contains_secret(matrix):
        raise RunnerError("Y_N_MATRIX_SECRET_FIELD")
    (output / "y-n-matrix-result.json").write_text(
        json.dumps(matrix, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print("SPEC180_CASE_RESULT status=PASS case=Y-N", flush=True)
    return 0


def run_minindn_case(case: str, output: Path, inputs: Mapping[str, Any]) -> int:
    if case == "Y-N":
        return _run_y_n_matrix(output, inputs)
    return _run_live_case_once(case, output, inputs)


@contextmanager
def _case_cancellation():
    """Turn operator/parent cancellation into the live driver's finally path.

    Ignore subsequent cancellation signals while unwinding owned resources;
    an external supervisor still owns the ultimate hard deadline. Restore the
    caller's handlers on every exit, including failed setup or cleanup.
    """
    previous = {}

    def cancel(signum, _frame):
        for value in previous:
            signal.signal(value, signal.SIG_IGN)
        raise RunnerError("CASE_CANCELLED:" + str(signum))

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.getsignal(signum)
            signal.signal(signum, cancel)
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Spec180 ACK-driven YOLO MiniNDN case")
    parser.add_argument("--case", required=True, choices=CASE_IDS)
    args = parser.parse_args(argv)
    try:
        # spec181 R004: refuse protected-epoch execution before any output
        # root or validation side effect while grant wiring is absent.
        _assert_grant_wiring_or_plaintext(os.environ)
    except RunnerError as exc:
        print("SPEC180_CASE_RESULT status=UNQUALIFIED error="
              + str(exc), flush=True)
        return 2
    try:
        output, inputs = validate_inputs(args.case, os.environ)
    except RunnerError as exc:
        # Input closure is a distinct state from a failed protocol run.  The
        # validation phase has not started NFD/SVS or any Provider, so callers
        # must be able to record this as an external-input wait and avoid
        # relabeling it as a negative NDNSF-DI result.
        print("SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT error="
              + str(exc), flush=True)
        return 78
    try:
        # This probe is deliberately outside run_minindn_case(): a split
        # host ABI is an input/readiness defect, not an executed NDNSF-DI
        # failure.  Report it before MiniNDN creates NFD or child processes.
        _validate_native_library_closure()
    except RunnerError as exc:
        print("SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT error="
              + str(exc), flush=True)
        return 78
    try:
        with _case_cancellation():
            return run_minindn_case(args.case, output, inputs)
    except RunnerError as exc:
        print("SPEC180_CASE_RESULT status=UNQUALIFIED error=" + str(exc), flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
