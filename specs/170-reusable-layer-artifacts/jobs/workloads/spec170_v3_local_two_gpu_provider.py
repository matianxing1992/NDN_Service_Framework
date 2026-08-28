#!/usr/bin/env python3
"""NDNSF Provider for the Spec170 one-Provider/two-GPU D2a gate."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import threading
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any


INDEPENDENT_DEVICES = {
    "/D2A/Independent/0": "cuda:0",
    "/D2A/Independent/1": "cuda:1",
}
LOCAL_RANK_DEVICES = {0: "cuda:0", 1: "cuda:1"}
LOCAL_RANK_INPUTS = {
    0: (1.0, 2.0, 3.0, 4.0),
    1: (10.0, 20.0, 30.0, 40.0),
}
ROLE_KEYS = (
    "/D2A/Independent/0", "/D2A/Independent/1",
    "/D2A/LocalGroup#0", "/D2A/LocalGroup#1",
)
UNSPLIT_ORACLE = tuple(
    left + right for left, right in zip(LOCAL_RANK_INPUTS[0], LOCAL_RANK_INPUTS[1])
)
ABSOLUTE_TOLERANCE = 1e-6
SERVICE = "/Inference/Spec170LocalTwoGpu"
MODEL_DIGEST = "sha256:" + "3" * 64
SIGNING_KEY = b"spec170-v3-local-two-gpu-provider-key"


class D2aIncompleteGroupError(RuntimeError):
    """The atomic D2a response cannot be produced from a partial local group."""


def _values(value: Mapping[str, object], label: str) -> list[float]:
    raw = value.get("values")
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes)):
        raise D2aIncompleteGroupError(f"{label} has no numeric values")
    try:
        return [float(item) for item in raw]
    except (TypeError, ValueError) as exc:
        raise D2aIncompleteGroupError(f"{label} has invalid numeric values") from exc


def build_d2a_final_result(
        *,
        provider: str,
        independent: Mapping[str, Mapping[str, object]],
        local_rank_results: Mapping[int, Mapping[str, object]],
) -> dict[str, object]:
    """Validate complete two-device execution before constructing a response."""
    for role, device in INDEPENDENT_DEVICES.items():
        if role not in independent:
            raise D2aIncompleteGroupError(f"missing independent role {role}")
        if independent[role].get("device") != device:
            raise D2aIncompleteGroupError(f"{role} executed on the wrong device")
        _values(independent[role], role)
    for rank, device in LOCAL_RANK_DEVICES.items():
        if rank not in local_rank_results:
            raise D2aIncompleteGroupError(f"missing local rank {rank}")
        if local_rank_results[rank].get("device") != device:
            raise D2aIncompleteGroupError(f"local rank {rank} executed on the wrong device")

    rank_values = {
        rank: _values(local_rank_results[rank], f"local rank {rank}")
        for rank in LOCAL_RANK_DEVICES
    }
    if any(len(values) != len(UNSPLIT_ORACLE) for values in rank_values.values()):
        raise D2aIncompleteGroupError("local collective result shape mismatch")
    maximum_error = max(
        abs(actual - expected)
        for values in rank_values.values()
        for actual, expected in zip(values, UNSPLIT_ORACLE)
    )
    if maximum_error > ABSOLUTE_TOLERANCE:
        raise D2aIncompleteGroupError(
            f"local collective unsplit oracle mismatch: {maximum_error}")

    return {
        "schema": "ndnsf-di-d2a-local-two-gpu-result-v1",
        "provider": provider,
        "complete": True,
        "collectiveBackend": "nccl",
        "independent": {key: dict(value) for key, value in independent.items()},
        "localGroup": {
            "rankResults": {str(key): dict(value)
                            for key, value in local_rank_results.items()},
            "unsplitOracle": list(UNSPLIT_ORACLE),
            "maxAbsoluteError": maximum_error,
        },
    }


class D2aGpuRuntime:
    """Exactly two CUDA devices, two ORT sessions, and one real NCCL group."""

    def __init__(self, artifact_root: str) -> None:
        import numpy as np
        import onnxruntime as ort
        import torch

        self.np = np
        self.ort = ort
        self.torch = torch
        if torch.cuda.device_count() != 2:
            raise RuntimeError(
                f"D2a requires exactly two visible GPUs, got {torch.cuda.device_count()}")
        if not torch.distributed.is_nccl_available():
            raise RuntimeError("D2a requires a PyTorch build with NCCL")
        if not hasattr(torch.cuda.nccl, "all_reduce"):
            raise RuntimeError("D2a torch.cuda.nccl.all_reduce is unavailable")
        if "CUDAExecutionProvider" not in ort.get_available_providers():
            raise RuntimeError("D2a requires ONNX Runtime CUDAExecutionProvider")
        model = Path(artifact_root) / "qwen-native-tracer-backbone.onnx"
        if not model.is_file():
            raise RuntimeError(f"missing D2a ONNX artifact: {model}")
        self.sessions = {
            index: self._load_session(model, index) for index in (0, 1)
        }
        for index in (0, 1):
            self._run_independent(index, warmup=True)

    def _load_session(self, model: Path, device: int):
        options = self.ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.add_session_config_entry("session.disable_cpu_ep_fallback", "1")
        session = self.ort.InferenceSession(
            str(model), sess_options=options,
            providers=[("CUDAExecutionProvider", {"device_id": device})],
        )
        providers = tuple(session.get_providers())
        if not providers or providers[0] != "CUDAExecutionProvider":
            raise RuntimeError(
                f"D2a ORT session did not select CUDA on device {device}: {providers}")
        return session

    def _run_independent(self, index: int, *, warmup: bool = False) -> list[float]:
        session = self.sessions[index]
        image = self.np.zeros((1, 3, 2, 2), dtype=self.np.float32) if warmup else (
            self.np.arange(1, 13, dtype=self.np.float32).reshape(1, 3, 2, 2) / 16.0)
        output = session.run(None, {session.get_inputs()[0].name: image})[0]
        return [float(value) for value in self.np.asarray(
            output, dtype=self.np.float32).reshape(-1)]

    def run_independent(self, index: int) -> list[float]:
        return self._run_independent(index)

    def run_local_collective(self) -> dict[int, list[float]]:
        tensors = [
            self.torch.tensor(
                LOCAL_RANK_INPUTS[rank], dtype=self.torch.float32,
                device=f"cuda:{rank}")
            for rank in (0, 1)
        ]
        self.torch.cuda.nccl.all_reduce(tensors)
        for rank in (0, 1):
            self.torch.cuda.synchronize(rank)
        return {
            rank: [float(value) for value in tensors[rank].cpu().tolist()]
            for rank in (0, 1)
        }


class D2aSessionCoordinator:
    """Atomic per-request completion state for four local execution items."""

    def __init__(self, provider: str, runtime: D2aGpuRuntime) -> None:
        self.provider = provider
        self.runtime = runtime
        self._lock = threading.RLock()
        self._states: dict[str, dict[str, Any]] = {}

    def _state(self, session_id: str) -> dict[str, Any]:
        return self._states.setdefault(session_id, {
            "independent": {}, "arrivedRanks": set(), "rankResults": {},
            "collectiveRunning": False, "assignmentStarted": False,
            "failed": "", "published": False,
        })

    def fail(self, session_id: str, reason: str) -> None:
        with self._lock:
            self._state(session_id)["failed"] = reason

    def run_role(self, session_id: str, role_key: str) -> None:
        if role_key in INDEPENDENT_DEVICES:
            index = int(role_key.rsplit("/", 1)[1])
            values = self.runtime.run_independent(index)
            with self._lock:
                state = self._state(session_id)
                if not state["failed"]:
                    state["independent"][role_key] = {
                        "device": f"cuda:{index}", "values": values,
                    }
            return
        if not role_key.startswith("/D2A/LocalGroup#"):
            raise RuntimeError(f"unexpected D2a role key: {role_key}")
        rank = int(role_key.rsplit("#", 1)[1])
        run_collective = False
        with self._lock:
            state = self._state(session_id)
            if state["failed"]:
                return
            state["arrivedRanks"].add(rank)
            if (state["arrivedRanks"] == {0, 1}
                    and not state["collectiveRunning"]
                    and not state["rankResults"]):
                state["collectiveRunning"] = True
                run_collective = True
        if run_collective:
            results = self.runtime.run_local_collective()
            with self._lock:
                state = self._state(session_id)
                if not state["failed"]:
                    state["rankResults"] = {
                        rank: {"device": f"cuda:{rank}", "values": values}
                        for rank, values in results.items()
                    }
                state["collectiveRunning"] = False

    def run_assigned_roles(
            self, session_id: str, assigned_roles: Sequence[str],
    ) -> tuple[str, ...]:
        """Execute one Provider-scoped assignment set exactly once.

        NDNSF groups every role assigned to one Provider into one Selection
        assignment set and invokes the Provider handler once.  The handler,
        rather than Core, is responsible for executing all local roles.
        """
        roles = tuple(str(role) for role in assigned_roles)
        if not roles or any(role not in ROLE_KEYS for role in roles):
            raise RuntimeError("D2A_ASSIGNMENT_ROLE_SET_INVALID")
        with self._lock:
            state = self._state(session_id)
            if state["failed"] or state["assignmentStarted"]:
                return ()
            state["assignmentStarted"] = True
        completed: list[str] = []
        for role in roles:
            self.run_role(session_id, role)
            completed.append(role)
        return tuple(completed)

    def take_final_result(self, session_id: str) -> dict[str, object] | None:
        with self._lock:
            state = self._state(session_id)
            if state["failed"] or state["published"]:
                return None
            if (set(state["independent"]) != set(INDEPENDENT_DEVICES)
                    or set(state["rankResults"]) != set(LOCAL_RANK_DEVICES)):
                return None
            result = build_d2a_final_result(
                provider=self.provider,
                independent=state["independent"],
                local_rank_results=state["rankResults"],
            )
            state["published"] = True
            return result


def _parse_request(payload: bytes) -> dict[str, Any]:
    value = json.loads(bytes(payload).decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("D2a request must be a JSON object")
    return value


def _projection_role_keys(projection) -> tuple[str, ...]:
    counts = {
        role.role: sum(other.role == role.role for other in projection.roles)
        for role in projection.roles
    }
    return tuple(
        role.role if counts[role.role] == 1 else f"{role.role}#{role.rank}"
        for role in projection.roles
    )


def main() -> int:
    from ndnsf import AckDecision, ServiceProvider
    from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3
    from ndnsf_distributed_inference.sdk.placement import (
        ExecutionDisposition,
        ProviderSelectionProjectionV3,
        UNBOUND_GRAPH_DIGEST_V3,
    )

    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--bootstrap-token", required=True)
    parser.add_argument("--service", default=SERVICE)
    args = parser.parse_args()

    runtime = D2aGpuRuntime(args.artifact_root)
    provider = ServiceProvider(
        provider_id=args.provider.rstrip("/").rsplit("/", 1)[-1],
        provider_prefix="/NDNSF-DI/Tracer/provider",
        group=args.group, controller=args.controller,
        trust_schema=args.trust_schema, bootstrap_token=args.bootstrap_token,
    )
    role_keys = ROLE_KEYS
    signer_key_id = "sha256:" + hashlib.sha256(SIGNING_KEY).hexdigest()
    issuer = DIProviderOfferIssuerV3(
        provider=args.provider, service=args.service,
        boot_epoch=provider.provider_boot_epoch,
        devices=("cuda:0", "cuda:1"), signer_key_id=signer_key_id,
        sign_offer_digest=lambda digest: hmac.new(
            SIGNING_KEY, digest.encode("utf-8"), hashlib.sha256).hexdigest(),
    )
    coordinator = D2aSessionCoordinator(args.provider, runtime)

    def make_ack(context, payload: bytes) -> AckDecision:
        request = _parse_request(payload)
        deadline_ms = int(request.get("deadlineMs", 0) or 0)
        if (str(request.get("requestId", "")) == ""
                or deadline_ms <= int(time.time() * 1000)):
            return AckDecision(status=False, message="D2A_REQUEST_INVALID")
        decision = issuer.issue(
            request_id=str(request["requestId"]),
            attempt=int(request.get("attempt", 1)),
            model_digest=str(request.get("model", {}).get(
                "identityDigest", MODEL_DIGEST)),
            graph_digest=UNBOUND_GRAPH_DIGEST_V3,
            deadline_ms=deadline_ms, accepted_roles=role_keys,
            backends=("onnxruntime-cuda",),
            execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
            preparation_accepted=True,
        )
        print(
            f"SPEC170_D2A_PROVIDER_ACK provider={args.provider} "
            f"requestId={request['requestId']} status={str(decision.status).lower()} "
            "devices=cuda:0,cuda:1",
            flush=True,
        )
        return decision

    def handle(context, payload: bytes) -> None:
        request = _parse_request(payload)
        session_id = context.session_id
        role_key = context.role
        try:
            projection = ProviderSelectionProjectionV3.from_bytes(
                context.assignment.assignment_payload)
            requested_map = request.get("providerByRole", {})
            native_role_map = dict(context.assignment.role_providers)
            assigned_roles = tuple(
                role for role in role_keys
                if native_role_map.get(role) == args.provider
            )
            if (projection.provider != args.provider
                    or projection.request_id != session_id
                    or tuple(_projection_role_keys(projection)) != role_keys
                    or native_role_map != requested_map
                    or requested_map != {key: args.provider for key in role_keys}
                    or context.assignment.role != role_key
                    or role_key not in role_keys
                    or assigned_roles != role_keys):
                raise RuntimeError("D2A_SELECTION_BINDING_MISMATCH")
            roles_to_run = assigned_roles
            if request.get("negativeCase") == "missing_rank":
                roles_to_run = tuple(
                    role for role in assigned_roles
                    if role != "/D2A/LocalGroup#1"
                )
            completed_roles = coordinator.run_assigned_roles(
                session_id, roles_to_run)
            for completed_role in completed_roles:
                print(
                    f"SPEC170_D2A_ROLE_COMPLETE requestId={session_id} "
                    f"role={completed_role}",
                    flush=True,
                )
            if request.get("negativeCase") == "missing_rank":
                coordinator.fail(session_id, "D2A_LOCAL_RANK_1_LOST")
                print(
                    f"SPEC170_D2A_LOCAL_RANK_LOST requestId={session_id} rank=1 "
                    "finalResponse=0",
                    flush=True,
                )
                context.fail("D2A_LOCAL_RANK_1_LOST")
                return
            result = coordinator.take_final_result(session_id)
            if result is not None:
                context.publish_final_response(json.dumps(
                    result, sort_keys=True, separators=(",", ":")).encode("utf-8"))
                print(
                    f"SPEC170_D2A_PROVIDER_RESPONSE requestId={session_id} "
                    "independentRoles=2 localRanks=2 collective=nccl complete=true",
                    flush=True,
                )
        except Exception as exc:  # noqa: BLE001
            coordinator.fail(session_id, str(exc))
            context.fail("D2A_EXECUTION_FAIL:" + str(exc))
            print(
                f"SPEC170_D2A_EXECUTION_FAIL requestId={session_id} "
                f"role={role_key} error={exc}",
                flush=True,
            )

    provider.add_collaboration_handler(
        args.service, list(role_keys), handle,
        ack_handler=make_ack, include_ack_context=True,
    )
    print(
        f"SPEC170_D2A_PROVIDER_READY provider={args.provider} "
        "devices=cuda:0,cuda:1 backend=onnxruntime-cuda "
        "collectiveBackend=nccl cpuFallbackUsed=false placementProfile=DI_PLACEMENT_V3",
        flush=True,
    )
    return provider.run()


if __name__ == "__main__":
    raise SystemExit(main())
