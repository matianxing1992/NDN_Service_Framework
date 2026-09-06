# Spec175 Local Closure Checklist

## Scope

- [x] Spec175 owns the generic streamed-invocation contract and the Qwen
  stateful generation path that uses it.
- [x] Spec175 ends at current-source unit, integration, and CPU/MiniNDN local
  qualification.
- [x] YOLO ACK-driven integration, SIF construction, CUDA execution,
  TigerCluster deployment, and cross-model qualification are transferred to
  Spec180.
- [x] Historical SIF and Tiger results are diagnostic provenance only and
  cannot close an active Spec175 requirement.
- [x] Timing is diagnostic; Spec175 makes no throughput or latency claim.

## T020 — design-code convergence

- [ ] The accepted streamed invocation, Qwen prefill/decode, continuation,
  state ownership, security, and terminal-outcome contracts are frozen.
- [ ] CodeGraph plus exact source inspection maps each requirement to the real
  public entry point, production caller, runtime owner, and evidence path.
- [ ] Every semantic, security, wiring, or evidence discrepancy has an owner
  task and focused regression.
- [ ] `audit.md` reports a fresh `PASS` after the final behavior-affecting
  change.

## T021 — complete local suites

- [ ] Relevant C++ unit and integration suites pass from one source identity.
- [ ] Relevant Python suites pass from the same source identity.
- [ ] Unary and Targeted compatibility, ordering, one terminal outcome,
  cancellation, replay rejection, Qwen state ownership, and continuation are
  covered.
- [ ] The deployed-runtime dependency check rejects runtime
  PyTorch/Transformers ownership.

## T022 — representative CPU/MiniNDN flows

- [ ] A cold Qwen request completes Request, ACK closure, Selection, prefill,
  decode, ordered Tokens, and one terminal Response.
- [ ] A same-conversation continuation reuses only compatible role-local state.
- [ ] A mismatched continuation fails or takes the explicit full-context
  fallback; it never silently reuses incompatible state.
- [ ] Real NFD, NDN-SVS, identities, routes, all child exits, result oracles,
  and bounded cleanup are recorded.

## T023 — closure and handoff

- [ ] One closure record binds source, effective configuration, commands,
  manifests, test results, child exits, and cleanup.
- [ ] The verdict is exactly `LOCAL_FUNCTIONAL_PASS` or `LOCAL_UNQUALIFIED`.
- [ ] `handoff-to-spec180.md` freezes consumed interfaces and lists every
  deferred obligation.
- [ ] The Spec175 closure identity is recorded as Spec180's baseline input;
  later Spec180 changes must be re-audited and re-tested by Spec180 rather than
  presented as Spec175 evidence.

## Current state

Scope and handoff structure are frozen. T020--T023 remain pending until fresh
current-source evidence is recorded.
