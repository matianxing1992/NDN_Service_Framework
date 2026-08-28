# Research: Real DI Validation Gates

## Executive Decision

The project needs a deployment-evidence boundary, not more smoke checks.
Spec 165 therefore makes fidelity explicit, reuses the real MiniNDN Qwen path,
runs the same workload in the candidate container, and separates renewable
inactivity detection from an immutable safety timeout.

## Decision 1: Evidence Declares Fidelity

### Decision

Every case emits a versioned fidelity record. Mandatory policy is evaluated
from fields in that record, never from a filename, marker string, or the mere
existence of output.

### Rationale

The previous suite mixed static checks, fake payloads, host processes,
MiniNDN, and containers. All are useful, but a common PASS label erased the
distinction between implementation-level and deployment-level evidence.

### Rejected alternatives

- Inferring fidelity from script names: names drift and are not attestations.
- Treating an unavailable dependency as a skip: a mandatory deployment
  prerequisite is unsatisfied.
- Allowing old evidence to satisfy a new run: this permits cross-revision and
  cross-configuration substitution.

## Decision 2: Use the Frozen Local Qwen3 Snapshot

### Decision

The default workload uses `Qwen/Qwen3-0.6B` revision
`e6de91484c29aa9480d55605af694f39b081c455`, resolved locally with network
download disabled. A content manifest/digest is computed before launch.

### Rationale

It is the smallest currently prepared Qwen3 artifact in this workspace and has
already been used by Spec 163. Pinning both repository name and immutable
revision enables reuse while preventing same-name model ambiguity.

### Rejected alternatives

- `revision=main`: mutable and cannot prove later reuse.
- Fake logits or expected-token fixtures: useful unit evidence but not model
  execution.
- Download during the gate: makes the result dependent on mutable external
  availability and can exhaust local storage.

## Decision 3: Extend the Existing Real MiniNDN Harness

### Decision

Extend `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` to consume a frozen
workload manifest and emit the required per-invocation evidence. Do not create
a second simulated orchestration path.

### Rationale

The harness already launches MiniNDN, Controller, User, and Provider processes.
Reusing it exercises the production transport boundary and makes failures
visible where they occur.

### Rejected alternatives

- Direct Python calls between stages: bypasses the network and protocol.
- A one-token correctness probe: cannot show sustained generation, token
  timing, or distributions.
- One successful prompt: hides request-state leakage and warm-cache behavior.

## Decision 4: One Workload Manifest for Host and Container

### Decision

Gate B and Gate C consume the same canonical workload manifest and emit the
same result schema. The container gate adds immutable image identity, mounts,
limits, exit code, and OOM state.

### Rationale

If the host and container choose their own prompts, model, counts, or minimum
tokens, container success does not validate the artifact that will actually be
deployed.

### Rejected alternatives

- `/bin/true`, socket, or startup-only container checks: lower fidelity.
- Container-specific smaller workload: not comparable.
- Skipping when no candidate image is configured: violates fail-closed
  deployment authorization.

## Decision 5: Preserve One Invocation Identity

### Decision

The User creates the canonical request ID. Every admitted ACK, Selection,
operation status, intermediate execution event, and Response must also bind
attempt, plan/selection identity, and model identity.

### Rationale

Request-ID equality alone does not prevent an event from an old retry, plan, or
model instance from mutating the current request.

### Rejected alternatives

- Generating a new ID at each layer: destroys end-to-end correlation.
- Comparing only request ID: insufficient across retries and replanning.
- Dropping rejected events: prevents proof that negative cases were handled.

## Decision 6: Renewable Idle, Immutable Hard Deadline

### Decision

Each operation owns:

- an idle deadline renewed only by authenticated, correctly bound,
  monotonically advancing progress; and
- an absolute deadline fixed at operation start.

Hard expiry has precedence when both boundaries are reached. Terminal state is
first-terminal-wins. Tests inject a deterministic monotonic clock.

### Rationale

A fixed 600-second timeout kills healthy long transfers or model preparation.
An endlessly renewable timeout permits a stuck or malicious provider to hold
the request forever.

### Rejected alternatives

- Fixed timeout only: confuses slow progress with a stall.
- Heartbeat-only renewal: liveness is not useful progress.
- Progress-only renewal without hard cap: unbounded resource retention.
- Wall-clock tests: slow and nondeterministic at boundary races.

## Decision 7: Ownership Boundary

### Decision

NDNSF owns the generic authenticated operation-status structure, binding
checks, monotonic admission, and deadline semantics. NDNSF-DI owns model
identity, stage meaning, generated-token evidence, and inference policy.

### Rationale

Storage, preparation, and other collaborations need progress deadlines, while
Qwen-specific phases must not leak into the base service framework.

### Rejected alternatives

- Qwen-specific status types in NDNSF: binds the framework to LLM inference.
- A DI-only unverified heartbeat channel: duplicates and weakens the existing
  authenticated collaboration path.

## Decision 8: TigerCluster Is a Later Validity Stage

### Decision

The default local command never submits TigerCluster jobs. It emits
`externalValidationAuthorized=true` only after Gate A-D pass. Remote submission
still requires an explicit operator command.

### Rationale

Local correctness must expose protocol, identity, packaging, and liveness
errors before scarce multi-node GPU resources are used.

### Rejected alternatives

- Automatically submitting after local PASS: changes external state without a
  separate operator decision.
- Using TigerCluster to discover local harness errors: slow and expensive.

## Research Verdict

The plan is necessary and minimally scoped. It does not replace existing tests;
it makes their evidentiary limits explicit and adds the missing real deployment
proof. The main implementation risk is runtime cost, controlled through the
small frozen model and bounded correctness workload rather than by weakening
the gate.
