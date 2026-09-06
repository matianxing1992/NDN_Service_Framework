# Frozen Qwen Reference Contract v1

Spec180 does not define new Qwen behavior. This contract records the exact
Spec175 local subjects that are proposed for import into the Spec180 local
gate. They become qualification inputs only after the Spec175 handoff seals
the corresponding baseline; this file does not promote unsealed evidence.

## Identity

The local identities are deliberately the small Spec175 CPU/MiniNDN fixture,
not the large Tiger model. The machine-readable binding is
`contracts/qwen-reference-manifest-v1.json`.

- `Q-C`: Spec175 M01 on `spec175-tiny-causal-lm-v1`, including its
  model/revision, tokenizer, prompt digest, stop policy, and CPU ONNX Runtime
  identity.
- `Q-W`: Spec175 M11 on the same tiny model/tokenizer identity, including its
  parent checkpoint, role map, and context epoch.
- Tiger `QWEN-F` is a separate Qwen3.6-27B ONNX artifact identity and is never
  substituted for either local case.

The Spec175 Tiger workload file at
`packaging/ndnsf-di-container/jobs/spec175/workload.json` is a 27B, 64-token,
120-second workload. It is not the local Q-C/Q-W reference and must not be
used to silently change the local model, token cap, or deadline.

The imported local case sequence is explicit:

- `Q-C` executes one cold request from a clean process/state root and requires
  one prefill, automatic single-token decode, ordered Tokens, and one terminal
  Response.
- `Q-W` executes two valid turns in one authenticated conversation. The second
  turn binds the first turn's committed checkpoint and role-local state. The
  same child then submits one stale or mismatched checkpoint and requires a
  fail-closed rejection before model computation; that negative attempt is not
  counted as a successful generation.

## Frozen controls

The imported manifests MUST retain one 5000 ms initial SVS settle, 1500 ms ACK
timeout, 60000 ms request timeout, greedy decode, and at most 8 newly generated
Tokens per request. Any change to model, tokenizer, prompt, stop policy, role
state, timeout, backend, or continuation semantics creates a new source/workload
identity and invalidates the Spec180 local gate.

The Tiger `QWEN-F` manifest is a separate required input. It must be a signed,
content-addressed manifest naming the Qwen3.6-27B ONNX graph, every stage
object, source revision, tokenizer/chat-template identity, stop policy, and
object digests. Its signature must verify against the registered external
artifact authority in `model-manifest-trust-v1.md`; the YOLO catalogue signer
does not implicitly authorize it. A local tiny fixture, an exact-SIF runtime
smoke, or an unsigned profile value cannot satisfy this requirement.

## Ownership and evidence

Q-C and Q-W use the existing generic coordinator, native Provider, streaming
state, security, and terminal-response owners. T012 only registers immutable
references and digests; it does not add a second Qwen lifecycle or planner.
The manifest's evidence policy is reference registration only: it neither
inherits Spec175 execution evidence nor qualifies Qwen3.6-27B or Tiger.
Prompt and token payloads remain in the controlled runner and are represented in
candidate evidence only by approved digests and oracle summaries.
