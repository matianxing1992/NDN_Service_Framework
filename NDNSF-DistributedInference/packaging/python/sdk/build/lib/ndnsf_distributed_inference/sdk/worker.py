"""Versioned least-input worker envelopes for untrusted optimizer processes."""

from __future__ import annotations

import json
from typing import Any, Mapping


FORBIDDEN_INPUT_KEYS = frozenset({
    "prompt", "payload", "tensor", "token", "secret", "credential",
    "private_key", "decrypted_policy", "cross_tenant_data",
})


def least_input_projection(value: Mapping[str, Any]) -> dict[str, Any]:
    projected: dict[str, Any] = {}
    for key, item in value.items():
        if str(key).lower() in FORBIDDEN_INPUT_KEYS:
            continue
        if isinstance(item, Mapping):
            projected[str(key)] = least_input_projection(item)
        elif isinstance(item, (list, tuple)):
            projected[str(key)] = [
                least_input_projection(entry) if isinstance(entry, Mapping) else entry
                for entry in item
                if not isinstance(entry, Mapping) or least_input_projection(entry)
            ]
        elif isinstance(item, (str, int, float, bool, type(None))):
            projected[str(key)] = item
    return projected


def encode_worker_envelope(kind: str, payload: Mapping[str, Any],
                           decision_epoch: int) -> bytes:
    if decision_epoch <= 0:
        raise ValueError("worker envelope requires decision epoch")
    return json.dumps({
        "schema": "ndnsf-di-policy-worker-v1", "kind": kind,
        "decisionEpoch": decision_epoch,
        "payload": least_input_projection(payload),
    }, sort_keys=True, separators=(",", ":")).encode()


def decode_worker_envelope(wire: bytes) -> dict[str, Any]:
    payload = json.loads(bytes(wire).decode())
    if payload.get("schema") != "ndnsf-di-policy-worker-v1":
        raise ValueError("unsupported worker envelope")
    if int(payload.get("decisionEpoch", 0)) <= 0:
        raise ValueError("invalid worker decision epoch")
    return payload


__all__ = ["FORBIDDEN_INPUT_KEYS", "least_input_projection",
           "encode_worker_envelope", "decode_worker_envelope"]
