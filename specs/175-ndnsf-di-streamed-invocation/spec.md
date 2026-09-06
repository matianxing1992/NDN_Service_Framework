# Feature Specification: NDNSF-DI Streamed Invocation Local Closure

**Feature**: `175-ndnsf-di-streamed-invocation`
**Branch**: `Experimental`
**Status**: `FROZEN_LOCAL_FUNCTIONAL_PASS` at the named source identity; this
is not a pass for the current dirty working tree. No active Spec175 tasks
remain (handoff boundary reaffirmed in revision 105). Later Spec180 changes
are outside this closure and require their own convergence.
**Superseding qualification feature**: `specs/180-ack-driven-cross-model-qualification`

**Revision 110 boundary reaffirmation (2026-09-03)**: Spec175 has no open
implementation or experiment task. Its `LOCAL_FUNCTIONAL_PASS` is limited to
the frozen generic stream/Qwen CPU-MiniNDN source seal. The current delay is
owned by Spec180's ordered `G0-NATIVE → G0-CANDIDATE → G1-Y-A` gates; the
native-library closure and missing YOLO inputs must not be repaired by
reopening, rebuilding, or relabeling Spec175.

**Revision 108 boundary clarification (2026-09-03)**: Spec175 is not the
reason the current NDNSF-DI experiment is late. Its generic stream/Qwen
CPU-MiniNDN subject is closed at the sealed source identity above. The current
`DIRTY_INPUT_TREE` block is an anti-mixing guard because later Spec180 source
changes are present; it is not an unfinished Spec175 test. The missing real
experiment is owned by Spec180's G0/G1 queue: a trusted role-correct YOLO
candidate, registered signing material, bound Provider/model inputs, and one
live Y-A terminal Response. Do not reopen or rebuild Spec175 to obtain that
evidence.

**Revision 109 handoff clarification (2026-09-03)**: A later Spec180 developer
Y-A attempt exposed a split native-library closure (`.local-boost171` versus
`/usr/local` `libndn-cxx`) and stopped with socket EOF before any request. This
is a Spec180 host-build/input defect, not evidence against the frozen Spec175
subject. Spec175 remains read-only; the receiving feature must rebuild its
complete NFD/NDN-SVS/NDNSF/NAC-ABE/Python closure and requalify it independently.

**Closure boundary (2026-09-03)**: T020--T023 close the named frozen source
subject at revision `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`, using
`evidence/t022-source-seal-current-20260902-r1.json` and the recorded T021/T022
evidence. The working tree now also contains Spec180 implementation and
qualification changes. Those later paths are not retroactively covered by the
Spec175 pass; Spec180 must bind its own source delta, rerun convergence, and
requalify its local paths before any promotion or expensive execution.

**Current-tree gate (2026-09-03)**: Running
`scripts/spec175_contract_gate.py` against the shared working tree returns
`BLOCKED` with `DIRTY_INPUT_TREE` because the tree contains later Spec180
edits. The exact path count is runtime evidence, not a fixed document value.
This is the expected result for a tree with a later Spec180 delta: it prevents
a new Spec175 promotion, but it does not invalidate the historical
`LOCAL_FUNCTIONAL_PASS` bound to the frozen source seal above. The current
tree must not be called a fresh Spec175 candidate; use the Spec180 source
delta and its own convergence gate instead.

**Operational closure rule**: Spec175 is a read-only local baseline, not an
active implementation queue. Do not reopen T020--T023, append new Tiger/SIF
tasks, or rerun its frozen campaign to diagnose a Spec180 change. Spec180 may
reuse the sealed contracts and reference cases, but every post-handoff source
delta and all current YOLO/Qwen/SIF/Tiger evidence belong only to Spec180.

**Revision 105 operating rule (2026-09-03)**: A `DIRTY_INPUT_TREE` result means
that the current tree is a different source subject; it does not mean “rerun
Spec175.” If a Spec175-owned source or contract must change, create a new source
seal under a new feature. During Spec180 work, this feature is used only as a
named contract/evidence reference and its results must never be relabeled as
YOLO, QWEN-F, SIF, CUDA, or Tiger evidence.

**Revision 104 handoff clarification (historical; superseded by revision 106,
2026-09-03)**: The absence of a current Spec180 experiment is not an
unfinished Spec175 obligation. Spec175's four closed tasks prove only the
named generic-stream/Qwen CPU-MiniNDN subject. At that time the current delay
also included a missing QWEN-F entrypoint. The entrypoint now exists, but the
Spec180 delay is still caused by its missing admissible YOLO candidate/input
bundle and missing signed production QWEN-F object set. A current-tree
`DIRTY_INPUT_TREE` result must remain a boundary check; it must not trigger
another Spec175 build or campaign.

## Explicit handoff diagnosis (revision 99)

Spec175 did not delay the current work: its named source subject is already
closed. The apparent delay comes from Spec180 inputs and ordering, not from a
missing Spec175 test. The shared working tree is intentionally rejected by the
Spec175 dirty-tree gate because it contains the later Spec180 delta; this is a
protection against mixing evidence, not a request to rerun Spec175.

The only valid receiving-feature entry is a new Spec180 candidate with its own
source seal, runtime configuration, model/package digests, and signing
material. Until that identity exists, no Spec175 result may be relabeled as a
YOLO, SIF, CUDA, or Tiger result.

## Final handoff correction (revision 100)

The frozen Spec175 result is complete for its declared CPU/MiniNDN subject.
Spec180's delay is not caused by an unfinished Spec175 task: the current
working tree is a different source subject and must be qualified independently.
The handoff supplies contracts and evidence references only; it does not claim
that the later YOLO runner, lifecycle trace, SIF, CUDA, or Tiger path has
executed.

## Read-only handoff verification (revision 101)

Spec175 remains complete for its frozen CPU/MiniNDN subject. The lifecycle
journal and ACK-driven runner changes made while implementing Spec180 are
post-handoff source changes; they are not retroactive Spec175 work and do not
invalidate the sealed local pass. Conversely, the absence of a live YOLO
trace, a trusted current candidate, or Tiger evidence is a Spec180 execution
block, not an unfinished Spec175 task. Keep the two evidence identities
separate and do not reopen this feature to repair the receiving feature.

## Revision 102 handoff boundary (historical; superseded by revision 103)

The delay in obtaining a current NDNSF-DI experiment is a Spec180
execution/input-ordering issue, not unfinished Spec175 work. Spec175 has one
frozen subject and one frozen evidence identity: generic streaming, stateful
Qwen, and their recorded CPU/MiniNDN production cases. It does not promise the
later YOLO ACK-driven path, a new source-tree build, a new ONNX candidate
export, a SIF, or TigerCluster execution.

The receiving feature must complete its own source-delta audit, role-correct
trusted package, runtime policy/key/model binding, real ACK-to-terminal-
Response request, and downstream local/SIF/Tiger gates. A Spec175 PASS,
focused seam test, or historical SIF cannot substitute for those steps. If
the frozen Spec175 source is changed, that is a new source subject requiring a
new seal; this closure must not be edited to absorb it.

## Revision 103: frozen-baseline operating rule

Spec175 is intentionally finished and read-only. Its `LOCAL_FUNCTIONAL_PASS`
is valid only for the source seal and CPU/MiniNDN evidence named above. The
active working tree contains later Spec180 changes, so a current-tree
`DIRTY_INPUT_TREE` result is expected and is not a request to rerun Spec175.
The exact dirty-path count is evidence produced by the gate at run time, not a
normative value in this document.

The handoff is complete only when the receiving feature owns all new work:

| Item | Spec175 action | Spec180 owner |
|---|---|---|
| Generic streamed/Qwen baseline | keep sealed; do not modify | consume as named reference |
| YOLO ACK-driven request path | out of scope | T002/T004--T011 |
| New model/package/signing inputs | out of scope | G0 candidate closure |
| SIF, CUDA, and Tiger execution | out of scope | T014--T020 |

If a later repair changes a Spec175-owned source or contract, open a new
source subject with a new seal instead of reopening T020--T023. This prevents
the same failure from being repaired twice under two feature identities.

## Why Spec180 evidence is not inherited

This frozen baseline is not the cause of the current delay. The current
working tree contains a later Spec180 source delta, so its historical
`LOCAL_FUNCTIONAL_PASS` cannot be applied to that tree. Spec180 now contains a
barriered MiniNDN driver and candidate-bound publication code, but no valid
current candidate has yet produced a real Y-A terminal Response. The remaining
work is to regenerate the package with the current `DetectShard0/1` vocabulary,
bind the Provider offer-signing key and local canonical model path, and run the
actual Controller/Repository/Provider/User exchange. These repairs and all new
evidence belong to Spec180; reopening this feature would create a second
source of truth and invalidate the sealed handoff.

### Baseline versus current qualification

The `LOCAL_FUNCTIONAL_PASS` label is deliberately scoped to the sealed
Spec175 source identity and its CPU/MiniNDN evidence. It means that the
model-neutral stream, Qwen state, security checks, and registered local cases
passed at that checkpoint; it does **not** mean that the later YOLO
ACK-driven path, a rebuilt SIF, CUDA execution, or TigerCluster has run.

The operational state is intentionally two-valued: **frozen subject = PASS**,
**shared current tree = BLOCKED until a new subject is sealed**. No task should
try to “fix” the latter by rerunning Spec175; doing so would mix Spec180
changes into the old evidence.

| Evidence layer | Spec175 status | Owner after handoff |
|---|---|---|
| Generic stream/Qwen contracts and local production cases | sealed pass at the named source revision | Spec175 (read-only baseline) |
| Current dirty-tree Spec180 changes | not covered by the seal | Spec180 convergence and tests |
| YOLO ACK/Selection/Provider/Response execution | not a Spec175 obligation | Spec180 T011 |
| SIF/CUDA/Tiger qualification | explicitly out of scope | Spec180 T016--T020 |

This table is an evidence boundary, not an additional task list. A result from
one row must never be relabeled as a result from another row.

## Objective

Complete and locally qualify the model-neutral streamed-invocation lifecycle
and the Qwen autoregressive adapter that uses it. Spec175 ends after its named
source identity passes design-code convergence, complete local unit/integration tests,
and the registered CPU/MiniNDN production-path cases. It does not build or
promote a SIF and does not run TigerCluster.

The active route is:

```text
freeze design and production path
  -> complete local suites
  -> two representative CPU/MiniNDN flows
  -> signed handoff to Spec180
```

## User Scenarios & Testing

### User Story 1 — Consume one bounded result stream (Priority: P1)

As an application developer, I can issue one authorized service request, receive
ordered intermediate events, and obtain exactly one authoritative terminal
Response without issuing a new service request for every event.

**Independent Test**: A deterministic local integration case emits multiple
events followed by one terminal Response and rejects duplicate, missing,
out-of-order, post-terminal, cancelled, and expired events.

**Acceptance Scenarios**:

1. **Given** a selected Provider and an accepted streaming request, **when** the
   Provider publishes events and completes, **then** the User delivers each
   event once in order and accepts exactly one matching terminal Response.
2. **Given** a cancelled, expired, or already-terminal invocation, **when** more
   events or a second terminal result arrive, **then** they cannot reopen or
   mutate the invocation.

### User Story 2 — Run autoregressive generation (Priority: P1)

As an NDNSF-DI application developer, I can execute one prefill followed by an
automatic single-token decode loop across a fixed one-Provider-per-role pipeline,
while each Provider retains only the model state for its own role.

**Independent Test**: A deterministic tiny causal model completes a cold
generation and a same-conversation continuation through the production
request/ACK/Selection and NDN dependency path.

**Acceptance Scenarios**:

1. **Given** a fresh request, **when** the pipeline runs, **then** it performs
   one prefill, repeats decode until the registered stop condition, publishes
   ordered Token events, and returns one terminal Response.
2. **Given** a valid conversation checkpoint, **when** a new turn extends that
   conversation, **then** each role validates and reuses its own state; a stale,
   missing, or mismatched checkpoint fails or takes only the explicitly recorded
   full-context fallback.

### User Story 3 — Hand off a locally qualified implementation (Priority: P2)

As an operator, I can identify exactly what Spec175 implemented, what current
local evidence proves, and which expensive or model-specific obligations move
to Spec180.

**Independent Test**: The closure record maps every active Spec175 requirement
to current-source code and accepted local evidence and contains no SIF, CUDA, or
Tiger qualification claim.

**Acceptance Scenarios**:

1. **Given** a design-code audit with no controlling gap, **when** the complete
   local suites and registered MiniNDN cases pass from the same source identity,
   **then** Spec175 closes as `LOCAL_FUNCTIONAL_PASS`.
2. **Given** any remaining local semantic, security, wiring, or evidence gap,
   **when** closure is attempted, **then** Spec175 remains `LOCAL_UNQUALIFIED`
   and the gap is not hidden by historical SIF or Tiger results.

### Edge Cases

- Event retention is exhausted before the User fetches an accepted event.
- A Provider publishes an event or terminal result for the wrong request,
  attempt, generation, plan, or Provider identity.
- A decode epoch is repeated, skipped, or delivered after cancellation.
- A role loses, evicts, or restores incompatible conversation state.
- Two turns attempt to extend the same conversation epoch concurrently.
- The current source differs from the source identity used by historical local
  evidence.

## Requirements

### Functional Requirements

- **FR-001** — **One request lifecycle.** A streamed invocation MUST perform one
  authorized Request/ACK/Selection lifecycle and MUST NOT create a new service
  request for each event or decode token.
- **FR-002** — **Ordered bounded delivery.** Events MUST be request- and
  attempt-bound, delivered exactly once in sequence, bounded by explicit
  retention/backpressure limits, and closed by exactly one terminal Response.
- **FR-003** — **Terminal safety.** Cancellation, deadline, error, and successful
  completion MUST be mutually terminal; late or duplicate messages MUST NOT
  reopen the invocation.
- **FR-004** — **Security preservation.** Request, ACK, Selection, intermediate
  event, dependency, feedback, and terminal paths MUST preserve the existing
  NDNSF authentication, authorization, confidentiality, token, and replay
  checks without a debug bypass.
- **FR-005** — **Autoregressive lifecycle.** The Qwen adapter MUST distinguish
  prefill from decode, execute one automatic decode loop to a registered stop
  condition, and expose ordered token events plus one terminal result.
- **FR-006** — **Provider-owned state.** Every selected Provider MUST own one
  complete execution role and retain only that role's complete KV and, where
  applicable, recurrent/convolution state. Live model state MUST NOT traverse
  NDN links during a healthy decode loop.
- **FR-007** — **Conversation continuation.** A same-conversation successor MUST
  bind the exact parent checkpoint, tokenizer/chat-template identity, role map,
  and context epoch. Concurrent or mismatched successors MUST fail closed; a
  full-context fallback MUST be explicit and distinguishable from reuse.
- **FR-008** — **Design-to-code convergence.** Before complete suites or MiniNDN
  are accepted, a code-aware design-to-code convergence audit MUST inspect the
  public entrypoints, production callers, Provider/User wiring, security
  owners, effective configuration, and evidence path. Any controlling gap MUST
  block qualification until repaired and re-audited.
- **FR-009** — **Current-source local qualification.** The same source identity
  MUST pass complete relevant unit/integration suites and the registered cold
  generation plus same-conversation MiniNDN cases with clean child exits and
  cleanup.
- **FR-010** — **Explicit transfer.** YOLO adapter migration, SIF construction,
  CUDA execution, TigerCluster deployment, and cross-model qualification MUST
  be recorded as Spec180 work and MUST NOT be claimed by Spec175.

### Key Entities

- **Streamed invocation**: One request identity, attempt, ordered event stream,
  terminal state, deadlines, and delivery metrics.
- **Generation epoch**: One prefill or decode step with exact input/output and
  predecessor lineage.
- **Provider role state**: The model state owned by one selected role and bound
  to a request or conversation checkpoint.
- **Conversation checkpoint**: The authenticated aggregate lineage used to
  continue one conversation safely.
- **Local closure record**: The current-source audit, test, MiniNDN, child-exit,
  cleanup, and handoff evidence for Spec175.

## Success Criteria

### Measurable Outcomes

- **SC-001**. All registered streamed-invocation unit and integration tests pass
  from one current source identity with zero unexpected failures.
- **SC-002**. The cold MiniNDN case records Request, ACK closure, Selection,
  every role activation, one prefill, the expected ordered token sequence, one
  terminal Response, all child exit statuses, and zero surviving owned
  processes.
- **SC-003**. The same-conversation MiniNDN case completes two turns and proves
  checkpoint continuity and role-local state reuse without sending model state
  over NDN; its registered stale/mismatch case fails closed.
- **SC-004**. The closure audit maps 100% of FR-001--FR-010 to production code
  and current evidence and returns `PASS` before `LOCAL_FUNCTIONAL_PASS` is
  recorded.
- **SC-005**. The handoff lists every deferred YOLO/SIF/CUDA/Tiger obligation
  and contains zero statements that upgrade historical diagnostics into current
  qualification evidence.

## Assumptions

- Existing unary and Targeted APIs remain unchanged and are regression-tested.
- `PreSplitFirstStrategy` and the one-Provider/one-complete-role invariant remain
  the default placement contract.
- Deterministic tiny ONNX fixtures prove protocol and state semantics locally;
  they do not prove real-model numerical correctness, CUDA execution, or
  performance.
- Spec180 consumes the frozen Spec175 interfaces rather than redesigning the
  generic streaming or Qwen state machines.

## Out of Scope

- YOLO-specific graph semantics or split selection.
- SIF build, exact-SIF replay, upload, remote mutation, Slurm submission, CUDA,
  or TigerCluster execution.
- Throughput, TTFT, TPOT, latency superiority, statistical repetition, or a
  performance matrix.
- Cross-Provider tensor parallelism, role replication, live state migration,
  or a new placement strategy.
