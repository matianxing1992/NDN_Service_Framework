"""Adapter-owned stateful ONNX decode contract.

The module contains no model exporter or framework import.  It is a small
transaction boundary used by the deployed ONNX Runtime adapter: a candidate
decode state is validated and admitted before it replaces the committed state.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from threading import RLock
from typing import Any, Callable, Mapping, Optional, Sequence


class DecodeStateContractError(ValueError):
    pass


def _digest(value: str, label: str) -> None:
    if not isinstance(value, str) or not value.startswith("sha256:") \
            or len(value) != len("sha256:") + 64:
        raise DecodeStateContractError(f"invalid {label} digest")


@dataclass(frozen=True)
class DecodeStateIdentityV1:
    model_digest: str
    graph_semantic_digest: str
    artifact_digest: str
    adapter_digest: str
    tokenizer_digest: str
    runner_digest: str
    role_name: str
    role_split_digest: str
    layer_range: tuple[int, int]
    prefix_digest: str
    prefix_token_count: int
    position_digest: str
    precision: str
    layout_digest: str
    state_schema_digest: str
    state_component_digests: tuple[str, ...]
    runtime_abi_digest: str
    security_domain_digest: str
    provider_identity: str
    provider_boot_id: str
    state_inference_epoch: int
    predecessor_inference_epoch: Optional[int]
    cache_epoch: int
    request_id: str
    attempt_epoch: int
    generation_id: str

    def validate(self) -> None:
        for value, label in (
            (self.model_digest, "model"),
            (self.graph_semantic_digest, "graph semantic"),
            (self.artifact_digest, "artifact"),
            (self.adapter_digest, "adapter"),
            (self.tokenizer_digest, "tokenizer"),
            (self.runner_digest, "runner"),
            (self.role_split_digest, "role split"),
            (self.prefix_digest, "prefix"),
            (self.position_digest, "position"),
            (self.layout_digest, "layout"),
            (self.state_schema_digest, "state schema"),
            (self.runtime_abi_digest, "runtime ABI"),
            (self.security_domain_digest, "security domain"),
        ):
            _digest(value, label)
        if (not self.request_id or not self.generation_id
                or not self.role_name or not self.provider_identity
                or not self.provider_boot_id or not self.precision):
            raise DecodeStateContractError("decode state identity is incomplete")
        if (len(self.layer_range) != 2 or self.layer_range[0] < 0
                or self.layer_range[1] <= self.layer_range[0]):
            raise DecodeStateContractError("decode state layer range is invalid")
        if (self.prefix_token_count < 0 or self.cache_epoch < 0
                or self.state_inference_epoch < 0
                or self.attempt_epoch < 1 or self.attempt_epoch > 2):
            raise DecodeStateContractError("decode state epoch is out of range")
        if self.state_inference_epoch == 0:
            if self.predecessor_inference_epoch is not None:
                raise DecodeStateContractError(
                    "prefill decode state must not name a predecessor")
        elif (self.predecessor_inference_epoch is None
              or self.predecessor_inference_epoch + 1
              != self.state_inference_epoch):
            raise DecodeStateContractError(
                "decode state predecessor epoch is not contiguous")
        if not self.state_component_digests:
            raise DecodeStateContractError("decode state component identity is missing")
        for digest in self.state_component_digests:
            _digest(digest, "state component")

    # Stable read-only aliases used by older adapter diagnostics.
    @property
    def role(self) -> str:
        return self.role_name

    @property
    def schema_digest(self) -> str:
        return self.state_schema_digest


@dataclass(frozen=True)
class StateComponent:
    name: str
    dtype: str
    shape: tuple[int, ...]
    digest: str

    def validate(self) -> None:
        if not self.name or not self.dtype or not self.shape \
                or any(int(dim) < 0 for dim in self.shape):
            raise DecodeStateContractError("invalid decode state component")
        _digest(self.digest, "component")


@dataclass(frozen=True)
class DecodeStateBundleV1:
    identity: DecodeStateIdentityV1
    full_attention_kv: tuple[StateComponent, ...]
    recurrent_convolution: tuple[StateComponent, ...]
    token_epoch: int = 0

    def validate(self) -> None:
        self.identity.validate()
        if not self.full_attention_kv:
            raise DecodeStateContractError("full-attention KV state is missing")
        if not self.recurrent_convolution:
            raise DecodeStateContractError(
                "linear-attention recurrent/convolution state is missing")
        names: set[str] = set()
        for component in (*self.full_attention_kv, *self.recurrent_convolution):
            component.validate()
            if component.name in names:
                raise DecodeStateContractError("duplicate decode state component")
            names.add(component.name)
        component_digests = tuple(component.digest for component in
                                  (*self.full_attention_kv,
                                   *self.recurrent_convolution))
        if component_digests != self.identity.state_component_digests:
            raise DecodeStateContractError(
                "decode state component identity does not match bundle")
        if self.token_epoch < 0:
            raise DecodeStateContractError("decode state token epoch is invalid")
        if self.token_epoch != self.identity.prefix_token_count:
            raise DecodeStateContractError(
                "decode state token epoch does not match accepted prefix")

    def digest(self) -> str:
        self.validate()
        text = repr((self.identity, self.full_attention_kv,
                     self.recurrent_convolution)).encode()
        return "sha256:" + hashlib.sha256(text).hexdigest()


class DecodeStateTransaction:
    """One candidate state update with an explicit downstream admission gate."""

    def __init__(self, committed: DecodeStateBundleV1) -> None:
        committed.validate()
        self._committed = committed

    @property
    def committed(self) -> DecodeStateBundleV1:
        return self._committed

    def apply(self, candidate: DecodeStateBundleV1,
              admit: Callable[[Mapping[str, object]], bool]) -> None:
        candidate.validate()
        current = self._committed.identity
        next_identity = candidate.identity
        # Every identity field except the accepted prefix/position/cache epoch
        # must remain byte-for-byte identical inside one attempt.  This avoids
        # accidentally treating a compatible-looking partial cache as a hit.
        immutable_fields = (
            "model_digest", "graph_semantic_digest", "artifact_digest",
            "adapter_digest", "tokenizer_digest", "runner_digest",
            "role_name", "role_split_digest", "layer_range", "precision",
            "layout_digest", "state_schema_digest", "state_component_digests",
            "runtime_abi_digest", "security_domain_digest", "provider_identity",
            "provider_boot_id", "request_id", "attempt_epoch", "generation_id",
        )
        for field_name in immutable_fields:
            if getattr(next_identity, field_name) != getattr(current, field_name):
                raise DecodeStateContractError(
                    f"candidate {field_name} binding mismatch")
        # A successful incremental transition must describe a new accepted
        # prefix and position. Reusing either digest would make a stale tensor
        # bundle look like a valid next-token state. The cache epoch identifies
        # the resident state store; it is not a per-token counter.
        if next_identity.prefix_digest == current.prefix_digest:
            raise DecodeStateContractError("candidate prefix digest did not advance")
        if next_identity.position_digest == current.position_digest:
            raise DecodeStateContractError("candidate position digest did not advance")
        if next_identity.cache_epoch != current.cache_epoch:
            raise DecodeStateContractError("candidate cache epoch changed")
        if (next_identity.state_inference_epoch
                != current.state_inference_epoch + 1
                or next_identity.predecessor_inference_epoch
                != current.state_inference_epoch):
            raise DecodeStateContractError(
                "candidate predecessor inference epoch is not contiguous")
        current_components = (
            *self._committed.full_attention_kv,
            *self._committed.recurrent_convolution,
        )
        candidate_components = (
            *candidate.full_attention_kv,
            *candidate.recurrent_convolution,
        )
        if tuple((item.name, item.dtype, item.shape) for item in candidate_components) != tuple(
            (item.name, item.dtype, item.shape) for item in current_components
        ):
            raise DecodeStateContractError("candidate state component layout changed")
        if next_identity.prefix_token_count != current.prefix_token_count + 1:
            raise DecodeStateContractError("candidate prefix is not contiguous")
        if candidate.token_epoch != self._committed.token_epoch + 1:
            raise DecodeStateContractError("candidate token epoch is not contiguous")
        if not admit({"stateDigest": candidate.digest(),
                      "tokenEpoch": candidate.token_epoch,
                      "stateHit": False,
                      "stateTransition": "recompute-or-decode"}):
            raise DecodeStateContractError("downstream admission rejected state")
        self._committed = candidate


@dataclass(frozen=True)
class StatefulOnnxIOContractV1:
    """Adapter-certified names for one persistent stateful ONNX stage."""

    input_names: tuple[str, ...]
    output_names: tuple[str, ...]
    state_input_names: tuple[str, ...]
    state_output_names: tuple[str, ...]
    state_families: tuple[str, ...] = (
        "attention_kv", "recurrent_state", "convolution_state")
    # These names are adapter-certified rather than inferred from arbitrary
    # caller feeds.  Empty names mean that a graph does not use the causal
    # position-input policy; a graph declaring one of them must declare the
    # complete attention-mask/position-IDs pair.
    position_input_policy: str = "qwen-causal-position-v1"
    attention_mask_input_name: str = "attention_mask"
    position_ids_input_name: str = "position_ids"
    cache_position_input_name: str = ""

    def __post_init__(self) -> None:
        # Keep hand-authored test/adapter contracts equivalent to the
        # session-derived form when the optional cache-position input is
        # present in the graph signature.
        if (not self.cache_position_input_name
                and "cache_position" in self.input_names):
            object.__setattr__(self, "cache_position_input_name", "cache_position")

    @classmethod
    def from_session(cls, session: Any) -> "StatefulOnnxIOContractV1":
        inputs = tuple(str(value.name) for value in session.get_inputs())
        outputs = tuple(str(value.name) for value in session.get_outputs())
        activation_inputs = {"hidden_in", "hidden_states_in"}
        state_inputs = tuple(
            name for name in inputs
            if name.endswith("_in") and name not in activation_inputs)
        # Activation bundles cross a Provider boundary as ``hidden_out`` (or
        # ``hidden_states_out``); they are not recurrent state and have no
        # successor input.  Every other ``*_out`` remains subject to the
        # one-to-one state mapping check in ``validate``.
        activation_outputs = {"hidden_out", "hidden_states_out"}
        state_outputs = tuple(
            name for name in outputs
            if name.endswith("_out") and name not in activation_outputs)
        has_positions = ("attention_mask" in inputs or
                         "position_ids" in inputs or
                         "cache_position" in inputs)
        contract = cls(
            inputs, outputs, state_inputs, state_outputs,
            position_input_policy=("qwen-causal-position-v1"
                                    if has_positions else ""),
            attention_mask_input_name=("attention_mask" if has_positions else ""),
            position_ids_input_name=("position_ids" if has_positions else ""),
            cache_position_input_name=("cache_position" if "cache_position" in inputs else ""),
        )
        contract.validate()
        return contract

    def validate(self) -> None:
        if not self.input_names or not self.output_names:
            raise DecodeStateContractError("stateful ONNX I/O is empty")
        if len(set(self.input_names)) != len(self.input_names):
            raise DecodeStateContractError("duplicate ONNX input name")
        if len(set(self.output_names)) != len(self.output_names):
            raise DecodeStateContractError("duplicate ONNX output name")
        for family in self.state_families:
            if (f"{family}_in" not in self.state_input_names
                    or f"{family}_out" not in self.state_output_names):
                raise DecodeStateContractError(
                    f"ONNX state family is incomplete: {family}")
        input_set = set(self.input_names)
        output_set = set(self.output_names)
        for output_name in self.state_output_names:
            if not output_name.endswith("_out"):
                raise DecodeStateContractError(
                    "ONNX state output has no successor input: " + output_name)
            successor = output_name[:-4] + "_in"
            if successor not in self.state_input_names or successor not in input_set:
                raise DecodeStateContractError(
                    "ONNX state output has no successor input: " + output_name)
        for input_name in self.state_input_names:
            if not input_name.endswith("_in"):
                raise DecodeStateContractError(
                    "ONNX state input has no predecessor output: " + input_name)
            predecessor = input_name[:-3] + "_out"
            if predecessor not in self.state_output_names or predecessor not in output_set:
                raise DecodeStateContractError(
                    "ONNX state input has no predecessor output: " + input_name)
        position_names = {
            name for name in (
                self.attention_mask_input_name,
                self.position_ids_input_name,
                self.cache_position_input_name,
            ) if name
        }
        declared_positions = input_set.intersection(position_names)
        if declared_positions:
            if (self.position_input_policy != "qwen-causal-position-v1"
                    or not self.attention_mask_input_name
                    or not self.position_ids_input_name
                    or self.attention_mask_input_name not in input_set
                    or self.position_ids_input_name not in input_set
                    or (self.cache_position_input_name
                        and self.cache_position_input_name not in input_set)):
                raise DecodeStateContractError(
                    "stateful ONNX causal position inputs are incomplete")

    def validate_feed(self, feed: Mapping[str, Any], *, incremental: bool) -> None:
        self.validate()
        required = set(self.input_names)
        missing = sorted(required.difference(feed))
        unknown = sorted(set(feed).difference(self.input_names))
        if missing:
            raise DecodeStateContractError(
                "missing stateful ONNX inputs: " + ",".join(missing))
        if unknown:
            raise DecodeStateContractError(
                "unknown stateful ONNX inputs: " + ",".join(unknown))

    def materialize_causal_position_inputs(
            self, *, logical_prefix_token_count: int,
            new_token_count: int) -> dict[str, Any]:
        """Create the sealed Qwen causal position tensors for one epoch.

        ``logical_prefix_token_count`` is the complete prefix represented by
        the candidate epoch; ``new_token_count`` is the suffix evaluated by
        this call.  This mirrors the C++ adapter policy and deliberately
        refuses to invent positions for a graph without certified inputs.
        """
        self.validate()
        prefix = int(logical_prefix_token_count)
        new = int(new_token_count)
        if prefix <= 0 or new <= 0 or new > prefix:
            raise DecodeStateContractError(
                "causal position token extent is invalid")
        if self.position_input_policy != "qwen-causal-position-v1":
            raise DecodeStateContractError(
                "causal position policy is not certified")
        try:
            import numpy as np
        except ImportError as exc:  # pragma: no cover - deployment guard
            raise DecodeStateContractError(
                "causal position materialization requires numpy") from exc
        first = prefix - new
        positions = np.arange(first, prefix, dtype=np.int64).reshape(1, new)
        result: dict[str, Any] = {
            self.attention_mask_input_name:
                np.ones((1, prefix), dtype=np.int64),
            self.position_ids_input_name: positions,
        }
        if self.cache_position_input_name:
            result[self.cache_position_input_name] = positions.reshape(new)
        return result


class PersistentStatefulOnnxSession:
    """Persistent ORT session wrapper with explicit prefill/decode phases."""

    def __init__(self, session: Any, contract: StatefulOnnxIOContractV1) -> None:
        contract.validate()
        self.session = session
        self.contract = contract
        # ORT sessions are reusable but a stateful decode transition is not
        # concurrently re-entrant: one call's outputs become the next call's
        # predecessor.  Serialize the complete run while leaving ownership
        # and admission to the surrounding Provider state transaction.
        self._run_lock = RLock()

    def _run(self, feed: Mapping[str, Any], *, incremental: bool) -> dict[str, Any]:
        self.contract.validate_feed(feed, incremental=incremental)
        with self._run_lock:
            values = self.session.run(
                list(self.contract.output_names), dict(feed))
        if len(values) != len(self.contract.output_names):
            raise DecodeStateContractError(
                "stateful ONNX output count does not match the contract")
        return dict(zip(self.contract.output_names, values))

    def prefill(self, feed: Mapping[str, Any]) -> dict[str, Any]:
        return self._run(feed, incremental=False)

    def decode(self, feed: Mapping[str, Any]) -> dict[str, Any]:
        return self._run(feed, incremental=True)

    def run(self, feed: Mapping[str, Any], *, incremental: bool) -> dict[str, Any]:
        """Run one serialized prefill/decode step through the contract.

        The explicit ``incremental`` flag keeps callers from accidentally
        bypassing the feed validation when they use a common loop for the
        initial prefill and subsequent one-token decode calls.
        """
        return self._run(feed, incremental=bool(incremental))


__all__ = [
    "DecodeStateContractError",
    "DecodeStateIdentityV1",
    "StateComponent",
    "DecodeStateBundleV1",
    "DecodeStateTransaction",
    "StatefulOnnxIOContractV1",
    "PersistentStatefulOnnxSession",
]
