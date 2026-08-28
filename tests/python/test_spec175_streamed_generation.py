from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.adapters.qwen.generation import (  # noqa: E402
    OnePlanGenerationLoop,
    activation_data_name,
    token_feedback_data_name,
)
from ndnsf_distributed_inference.app_sdk.placement import (  # noqa: E402
    AutomaticStreamingHandle,
)


PLAN = "sha256:" + "a" * 64


FIXTURE = ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"


def _tiny_plan(partition: str):
    """Build one real persistent-ORT role plan for the CPU oracle."""
    manifest = json.loads((FIXTURE / "manifest.json").read_text(encoding="utf-8"))
    sessions = [
        ort.InferenceSession(
            str(FIXTURE / role["path"]),
            providers=["CPUExecutionProvider"],
        )
        for role in manifest["partitions"][partition]
    ]
    states = []
    for session in sessions:
        states.append({
            value.name: np.zeros(tuple(value.shape), dtype=np.float32)
            for value in session.get_inputs()
            if value.name.endswith("_in") and value.name != "hidden_in"
        })
    counters = {"prefill": 0, "decode": 0}

    def run_roles(token: int, current_states):
        hidden = None
        logits = None
        next_states = []
        activations = {}
        for index, session in enumerate(sessions):
            feed = dict(current_states[index])
            if index == 0:
                feed["input_ids"] = np.asarray([[token]], dtype=np.int64)
            else:
                assert hidden is not None
                feed["hidden_in"] = hidden
            names = [value.name for value in session.get_outputs()]
            output = dict(zip(names, session.run(names, feed)))
            hidden = output.get("hidden_out")
            if index + 1 < len(sessions):
                activations[(
                    f"/LLM/Pipeline/Stage/{index}",
                    f"/LLM/Pipeline/Stage/{index + 1}",
                )] = hidden
            logits = output.get("logits", logits)
            next_states.append({
                name.replace("_out", "_in"): value
                for name, value in output.items()
                if name.endswith("_out") and name != "hidden_out"
            })
        assert logits is not None
        return int(np.argmax(logits[0, -1])), next_states, activations

    def prefill(prompt, epoch):
        assert epoch == 0
        counters["prefill"] += 1
        token = int(prompt[-1])
        next_token, next_states, activations = run_roles(token, states)
        return {"logits": [0] * next_token + [1],
                "role_states": next_states, "activations": activations,
                "next_token": next_token}

    def decode(state, epoch, token):
        assert epoch >= 1
        counters["decode"] += 1
        next_token, next_states, activations = run_roles(
            token, state["role_states"])
        return {"logits": [0] * next_token + [1],
                "role_states": next_states, "activations": activations,
                "next_token": next_token}

    return manifest, counters, prefill, decode


@pytest.mark.parametrize("partition", ["one-role", "two-role", "four-role"])
def test_one_plan_loop_runs_real_cpu_onnx_partition_once_per_token(partition: str) -> None:
    """I01-I03 CPU oracle: one plan/prefill, persistent stateful role decode."""
    manifest, counters, prefill, decode = _tiny_plan(partition)
    prompt = json.loads((FIXTURE / "prompts.json").read_text(encoding="utf-8"))["cases"]["normal"]
    expected = tuple(manifest["expectedOutputs"]["normal"])
    activation_names: list[str] = []
    events: list[bytes] = []
    terminals: list[tuple[bytes, str]] = []
    roles = tuple(
        f"/LLM/Pipeline/Stage/{index}"
        for index in range(len(manifest["partitions"][partition]))
    )
    providers = {role: f"/provider/{index}" for index, role in enumerate(roles)}

    # The fixture's graph exposes a deterministic argmax token.  Encode that
    # token as the one-hot logit result consumed by the production sampler.
    def decode_text(token_ids):
        return "".join(str(token) + " " for token in token_ids).strip()

    loop = OnePlanGenerationLoop(
        request_id="/request/spec175-cpu-" + partition,
        requester="/user/spec175",
        service="/LLM/Qwen",
        attempt_epoch=1,
        plan_digest=PLAN,
        generation_id="generation-" + partition,
        roles=roles,
        first_role=roles[0],
        final_role=roles[-1],
        max_generated_tokens=len(expected),
        eos_token_ids=(2,),
        decode_text=decode_text,
        prefill=prefill,
        decode=decode,
        provider_by_role=providers,
        publish_activation=lambda name, _from, _to, _payload: activation_names.append(name),
        publish_event=events.append,
        finish=lambda payload, reason: terminals.append((payload, reason)),
    )
    result = loop.run(prompt["inputIds"])

    assert result.token_ids == expected
    assert counters["prefill"] == 1
    assert counters["decode"] == len(expected) - 1
    assert len(events) == len(expected)
    assert len(terminals) == 1
    assert terminals[0][1] == "eos"
    assert all("/provider/" in name for name in activation_names)
    assert len(activation_names) == max(0, len(expected) * (len(roles) - 1))


def test_exact_internal_names_are_distinct_and_epoch_bound() -> None:
    activation = activation_data_name(
        producer="/provider/final", requester="/user/1",
        service="/LLM/Qwen", request_id="req-1", attempt_epoch=1,
        plan_digest=PLAN, generation_id="generation-1",
        from_role="/LLM/Pipeline/Stage/0",
        to_role="/LLM/Pipeline/Stage/1", inference_epoch=2)
    feedback = token_feedback_data_name(
        producer="/provider/final", requester="/user/1",
        service="/LLM/Qwen", request_id="req-1", attempt_epoch=1,
        plan_digest=PLAN, generation_id="generation-1",
        final_role="/LLM/Pipeline/Stage/1",
        first_role="/LLM/Pipeline/Stage/0", token_epoch=3)
    assert "/NDNSF/DI/ACTIVATION/" in activation
    assert "/NDNSF/DI/TOKEN-FEEDBACK/" in feedback
    assert activation != feedback
    assert activation.endswith("/2")
    assert feedback.endswith("/3")


def test_stream_handle_timing_summary_uses_real_callbacks_without_payloads() -> None:
    handle = AutomaticStreamingHandle(
        None, {}, logical_request_id="/request/timing",
        generation_id="01" * 16,
    )
    handle._accept_event(1, json.dumps({
        "schema": "GenerationTokenEventV1",
        "tokenId": 4,
        "tokenEpoch": 1,
        "acceptedPrefixDigest": "sha256:" + hashlib.sha256(
            b"4").hexdigest(),
    }).encode())
    handle._accept_event(1, json.dumps({
        "schema": "GenerationTokenEventV1",
        "tokenId": 5,
        "tokenEpoch": 2,
        "acceptedPrefixDigest": "sha256:" + hashlib.sha256(
            b"4,5").hexdigest(),
    }).encode())
    handle._complete(1, b'{"tokenIds":[4,5]}')
    summary = handle.timing_summary
    assert summary["eventCount"] == 2
    assert summary["ttftMs"] is not None
    assert len(summary["interTokenMs"]) == 1
    assert summary["completeTimestampUs"] is not None
    assert "tokenIds" not in summary
    assert "prompt" not in summary


def test_stream_handle_rejects_mismatched_wire_identity() -> None:
    errors: list[dict] = []
    handle = AutomaticStreamingHandle(
        None, {}, logical_request_id="/request/identity",
        generation_id="01" * 16, on_error=errors.append,
    )
    handle._attempt_request_ids[1] = "/request/identity"
    handle._accept_event(1, json.dumps({
        "schema": "GenerationTokenEventV1",
        "requestId": "/request/other",
        "generationId": "01" * 16,
        "tokenId": 4,
        "tokenEpoch": 1,
        "acceptedPrefixDigest": "sha256:" + hashlib.sha256(
            b"4").hexdigest(),
    }).encode())
    assert handle.stream_error["code"] == "StreamEventLineageMismatch"
    assert len(errors) == 1
    assert handle.stream_events == ()


def test_stream_handle_callback_exception_is_terminal() -> None:
    errors: list[dict] = []
    handle = AutomaticStreamingHandle(
        None, {}, logical_request_id="/request/callback",
        generation_id="02" * 16,
        on_event=lambda _payload: (_ for _ in ()).throw(RuntimeError("sink")),
        on_error=errors.append,
    )
    handle._accept_event(1, json.dumps({
        "schema": "GenerationTokenEventV1",
        "tokenId": 4,
        "tokenEpoch": 1,
        "acceptedPrefixDigest": "sha256:" + hashlib.sha256(
            b"4").hexdigest(),
    }).encode())
    assert handle.stream_error["code"] == "StreamCallbackFailed"
    assert len(errors) == 1


def test_stream_handle_error_callback_exception_is_contained() -> None:
    handle = AutomaticStreamingHandle(
        None, {}, logical_request_id="/request/error-callback",
        on_error=lambda _error: (_ for _ in ()).throw(RuntimeError("sink")),
    )
    handle._fail(1, {"code": "ProviderFailure", "message": "failed"})
    assert handle.stream_error["code"] == "ProviderFailure"
    assert handle.stream_complete is None


def test_one_plan_loop_emits_event_then_feedback_and_one_terminal() -> None:
    events: list[bytes] = []
    feedback: list[tuple[str, str, dict]] = []
    terminals: list[tuple[bytes, str]] = []
    calls = {"prefill": 0, "decode": 0}

    def decode_text(token_ids):
        return "".join({4: "A", 5: "B", 2: ""}.get(token, "?")
                       for token in token_ids)

    def prefill(prompt, epoch):
        assert prompt == b"prompt"
        assert epoch == 0
        calls["prefill"] += 1
        return {"logits": [0, 0, 0, 0, 10, 0]}

    def decode(state, epoch, token):
        assert epoch == calls["decode"] + 1
        calls["decode"] += 1
        return {"logits": [0, 0, 0, 0, 0, 10]
                if epoch == 1 else [0, 0, 10, 0, 0, 0]}

    loop = OnePlanGenerationLoop(
        request_id="req-1", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=1, plan_digest=PLAN, generation_id="generation-1",
        roles=("/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/1", max_generated_tokens=3,
        eos_token_ids=(2,), decode_text=decode_text, prefill=prefill,
        decode=decode, publish_event=events.append,
        publish_feedback=lambda name, role, value: feedback.append((name, role, value)),
        finish=lambda payload, reason: terminals.append((payload, reason)),
    )
    result = loop.run(b"prompt")
    assert calls == {"prefill": 1, "decode": 2}
    assert result.token_ids == (4, 5, 2)
    assert [json.loads(value)["tokenEpoch"] for value in events] == [1, 2, 3]
    assert [value[2]["tokenEpoch"] for value in feedback] == [1, 2, 3]
    assert len(terminals) == 1
    assert terminals[0][1] == "eos"
    assert json.loads(terminals[0][0])["requestId"] == "req-1"


def test_attempt_two_recomputes_prefix_and_emits_continuation_only() -> None:
    events: list[bytes] = []
    calls = {"prefill": 0, "decode": 0}

    def prefill(_prompt, _epoch):
        calls["prefill"] += 1
        return {"logits": [0, 0, 0, 0, 10, 0]}

    def decode(_state, epoch, _token):
        calls["decode"] += 1
        token = 5 if epoch == 1 else 2
        return {"logits": [10 if index == token else 0
                           for index in range(6)]}

    loop = OnePlanGenerationLoop(
        request_id="req-recovery", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=2, plan_digest=PLAN, generation_id="01" * 16,
        roles=("/LLM/Pipeline/Stage/0",),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/0", max_generated_tokens=3,
        eos_token_ids=(2,), decode_text=lambda ids: "".join(map(str, ids)),
        prefill=prefill, decode=decode, publish_event=events.append,
        committed_prefix_token_ids=(4, 5),
    )
    result = loop.run(b"original-prompt")

    assert calls == {"prefill": 1, "decode": 2}
    assert result.token_ids == (4, 5, 2)
    assert len(result.events) == 1
    assert [json.loads(value)["tokenEpoch"] for value in events] == [3]
    assert [json.loads(value)["tokenId"] for value in events] == [2]


def test_attempt_two_rejects_recomputed_prefix_mismatch() -> None:
    loop = OnePlanGenerationLoop(
        request_id="req-recovery-bad", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=2, plan_digest=PLAN, generation_id="02" * 16,
        roles=("/LLM/Pipeline/Stage/0",),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/0", max_generated_tokens=2,
        decode_text=lambda ids: "".join(map(str, ids)),
        prefill=lambda _prompt, _epoch: {"logits": [0, 0, 0, 0, 10]},
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        committed_prefix_token_ids=(3,),
    )
    with pytest.raises(ValueError, match="recomputed committed prefix"):
        loop.run(b"original-prompt")


def test_one_plan_loop_does_not_emit_after_cancellation() -> None:
    events: list[bytes] = []
    loop = OnePlanGenerationLoop(
        request_id="req-2", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=1, plan_digest=PLAN, generation_id="generation-2",
        roles=("/LLM/Pipeline/Stage/0",), first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/0", max_generated_tokens=2,
        decode_text=lambda ids: "x" * len(ids),
        prefill=lambda _prompt, _epoch: {"logits": [0, 1]},
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        publish_event=events.append, cancelled=lambda: True,
    )
    result = loop.run(b"prompt")
    assert result.finish_reason == "cancelled"
    assert not events


def test_one_plan_loop_stops_when_event_admission_is_rejected() -> None:
    feedback: list[tuple[str, str, dict]] = []
    finished: list[tuple[bytes, str]] = []
    loop = OnePlanGenerationLoop(
        request_id="req-event-reject", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=1, plan_digest=PLAN, generation_id="generation-reject",
        roles=("/LLM/Pipeline/Stage/0",),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/0", max_generated_tokens=2,
        eos_token_ids=(2,), decode_text=lambda ids: "x" * len(ids),
        prefill=lambda _prompt, _epoch: {"logits": [0, 1]},
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        publish_event=lambda _wire: 0,
        publish_feedback=lambda name, role, value: feedback.append((name, role, value)),
        finish=lambda payload, reason: finished.append((payload, reason)),
    )
    with pytest.raises(ValueError, match="event publication was rejected"):
        loop.run(b"prompt")
    assert not feedback
    assert not finished
    assert loop.detokenizer.token_ids == ()
    assert loop.detokenizer.text == ""


def test_one_plan_loop_rejects_terminal_completion_failure() -> None:
    loop = OnePlanGenerationLoop(
        request_id="req-finish-reject", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=1, plan_digest=PLAN, generation_id="generation-finish-reject",
        roles=("/LLM/Pipeline/Stage/0",),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/0", max_generated_tokens=1,
        eos_token_ids=(1,), decode_text=lambda ids: "x" * len(ids),
        prefill=lambda _prompt, _epoch: {"logits": [0, 1]},
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        finish=lambda _payload, _reason: False,
    )
    with pytest.raises(ValueError, match="stream completion was rejected"):
        loop.run(b"prompt")


def test_activation_names_use_selected_provider_identity() -> None:
    names: list[str] = []
    loop = OnePlanGenerationLoop(
        request_id="req-3", requester="/user/1", service="/LLM/Qwen",
        attempt_epoch=1, plan_digest=PLAN, generation_id="generation-3",
        roles=("/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/1", max_generated_tokens=1,
        decode_text=lambda ids: "x" * len(ids),
        prefill=lambda _prompt, _epoch: {
            "logits": [0, 1],
            "activations": {
                ("/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"): b"hidden",
            },
        },
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        provider_by_role={
            "/LLM/Pipeline/Stage/0": "/provider/selected-0",
            "/LLM/Pipeline/Stage/1": "/provider/selected-1",
        },
        publish_activation=lambda name, _from, _to, _payload: names.append(name),
    )
    loop.run(b"prompt")
    assert names
    assert names[0].startswith("/provider/selected-0/NDNSF/DI/ACTIVATION/")


def test_one_plan_loop_rejects_missing_selected_role_activation() -> None:
    loop = OnePlanGenerationLoop(
        request_id="req-missing-activation", requester="/user/1",
        service="/LLM/Qwen", attempt_epoch=1, plan_digest=PLAN,
        generation_id="03" * 16,
        roles=("/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"),
        first_role="/LLM/Pipeline/Stage/0",
        final_role="/LLM/Pipeline/Stage/1", max_generated_tokens=1,
        decode_text=lambda ids: "x" * len(ids),
        prefill=lambda _prompt, _epoch: {"logits": [0, 1]},
        decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
        provider_by_role={
            "/LLM/Pipeline/Stage/0": "/provider/0",
            "/LLM/Pipeline/Stage/1": "/provider/1",
        },
        publish_activation=lambda *_args: None,
    )
    with pytest.raises(ValueError, match="missing activation"):
        loop.run(b"prompt")


def test_one_plan_loop_rejects_incomplete_activation_provider_map() -> None:
    with pytest.raises(ValueError, match="Provider map must cover"):
        OnePlanGenerationLoop(
            request_id="req-provider-map", requester="/user/1",
            service="/LLM/Qwen", attempt_epoch=1, plan_digest=PLAN,
            generation_id="04" * 16,
            roles=("/LLM/Pipeline/Stage/0", "/LLM/Pipeline/Stage/1"),
            first_role="/LLM/Pipeline/Stage/0",
            final_role="/LLM/Pipeline/Stage/1", max_generated_tokens=1,
            decode_text=lambda ids: "x" * len(ids),
            prefill=lambda _prompt, _epoch: {"logits": [0, 1]},
            decode=lambda _state, _epoch, _token: {"logits": [0, 1]},
            provider_by_role={"/LLM/Pipeline/Stage/0": "/provider/0"},
            publish_activation=lambda *_args: None,
        )
