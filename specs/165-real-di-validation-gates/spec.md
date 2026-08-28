# Feature Specification: Real DI Validation Gates

**Feature Branch**: `165-real-di-validation-gates`

**Created**: 2026-07-30

**Status**: Draft

**Input**: User description: "Implement Gate A-D for real NDNSF-DI local
validation: fidelity metadata, default blocking minimal Qwen3 MiniNDN
multi-token multi-request request-ID continuity, real local Docker, and
authenticated progress-driven idle/hard deadlines; TigerCluster remains
deferred."

## User Scenarios & Testing

### User Story 1 - Trustworthy Default Validation (Priority: P1)

As an NDNSF developer, I run one documented default validation command and can
tell exactly which components were real, simulated, skipped, or unavailable.
The command fails closed when a mandatory real-deployment gate is skipped,
replaced by a fake workload, or produces incomplete evidence.

**Why this priority**: Ambiguous PASS semantics are the primary reason defects
escaped local testing and first appeared on TigerCluster.

**Independent Test**: Run the aggregate gate against fixtures representing a
real pass, a skipped mandatory case, a fake-model substitution, and incomplete
metadata. Only the complete real case may pass.

**Acceptance Scenarios**:

1. **Given** every mandatory local gate produced complete, internally
   consistent evidence, **When** the aggregate result is evaluated, **Then**
   the suite passes and lists each real and simulated component.
2. **Given** any mandatory gate is skipped or reports fake weights, **When**
   the aggregate result is evaluated, **Then** the suite fails and names the
   unmet requirement.
3. **Given** a legacy unit or fixture test passes, **When** no corresponding
   real-model gate passes, **Then** the legacy result cannot satisfy the
   real-deployment requirement.

---

### User Story 2 - Real Minimum Qwen3 MiniNDN Proof (Priority: P1)

As an NDNSF-DI developer, I can run the smallest supported Qwen3 model through
the real MiniNDN collaboration path with several providers, generate multiple
tokens for multiple requests, and obtain complete answers and latency
distributions rather than a single top-token comparison.

**Why this priority**: The current optional single-token proof does not exercise
autoregressive generation, repeated requests, model residency, request
identity continuity, or distributional evidence.

**Independent Test**: In MiniNDN, run two real prompts, one warmup and at least
three measured invocations per prompt, through one User and at least three
Provider roles. Require immutable model identity, multi-token output, complete
request lineage, and per-request metrics.

**Acceptance Scenarios**:

1. **Given** the immutable minimum Qwen3 artifact is locally available,
   **When** the default MiniNDN gate runs, **Then** it loads real weights,
   executes at least three provider roles, and generates at least eight new
   tokens for every measured request.
2. **Given** two prompts, **When** each is warmed once and measured at least
   three times, **Then** the evidence retains all complete answers, TTFT,
   per-token latency, total latency, generated-token count, and tokens/s.
3. **Given** a request identifier established by the User, **When** Request,
   ACK, Selection, operation-status, intermediate execution, and Response
   events are admitted, **Then** every event is bound to that same identifier,
   attempt, plan identity, and model identity.
4. **Given** an event with a mismatched request identifier, attempt, model
   identity, or plan identity, **When** it is received, **Then** it is rejected
   and cannot extend a deadline or complete the request.
5. **Given** no GPU is available, **When** the gate runs in an explicitly
   declared CPU profile, **Then** the result may prove functional correctness
   but cannot claim GPU correctness or performance.

---

### User Story 3 - Candidate Container Runs the Same Workload (Priority: P2)

As a release engineer, I can run the same minimum-Qwen3 business path inside
the candidate local container with explicit memory limits. A container that
only starts, exposes an NFD socket, runs `/bin/true`, or uses fake bytes cannot
satisfy this gate.

**Why this priority**: Image and socket smoke tests do not prove that the
packaged runtime can execute the application path later submitted to
TigerCluster.

**Independent Test**: Execute the real-model workload in the candidate
container, compare its evidence contract with the MiniNDN gate, and inject a
fake-workload declaration to prove fail-closed behavior.

**Acceptance Scenarios**:

1. **Given** a candidate image and the immutable local model artifact, **When**
   the container gate runs under explicit memory and swap limits, **Then** the
   same real-model, multi-request evidence contract is produced.
2. **Given** the container exits successfully after only a startup/socket
   check, **When** the aggregate gate evaluates it, **Then** the real-container
   requirement remains unsatisfied.
3. **Given** an OOM kill, silent backend fallback, missing model hash, or fake
   payload, **When** the container evidence is evaluated, **Then** the gate
   fails with the exact boundary violation.

---

### User Story 4 - Progress-Driven Long Operations (Priority: P2)

As a requester, I can distinguish a slow but advancing model transfer or load
from a stalled operation. Authenticated forward progress renews only an idle
deadline, while an absolute hard deadline remains non-renewable.

**Why this priority**: A fixed 600-second timeout conflates legitimate large
artifact progress with deadlock and encourages unsafe timeout inflation.

**Independent Test**: Use a deterministic clock to exercise advancing,
duplicate, reordered, forged, stalled, cancelled, completed, and hard-cap
sequences without waiting in real time.

**Acceptance Scenarios**:

1. **Given** valid monotonically increasing progress before the idle deadline,
   **When** it is admitted, **Then** the idle deadline advances but never
   beyond the absolute deadline.
2. **Given** duplicate, reordered, unauthenticated, wrong-request, wrong-attempt,
   or invalid progress, **When** it is received, **Then** it does not renew any
   deadline.
3. **Given** no valid progress before the idle deadline, **When** the clock
   reaches that deadline, **Then** the operation becomes STALLED and releases
   or compensates bounded resources.
4. **Given** continuous valid progress, **When** the absolute hard deadline is
   reached, **Then** the operation terminates and cannot be kept alive by
   heartbeat traffic.
5. **Given** a terminal completion, failure, cancellation, or timeout, **When**
   later progress arrives, **Then** first-terminal-wins semantics reject it.

### Edge Cases

- A model returns EOS before the minimum evidence token count.
- A provider restarts and changes boot epoch during preparation.
- Progress increases sequence but not completed bytes or phase.
- Total byte count is initially unknown and later becomes known.
- A request is cancelled while a final token or Response is in flight.
- A cached model name matches but its content hash differs.
- CPU execution is explicitly selected but evidence claims GPU execution.
- One measured request succeeds while another is skipped or partially logged.
- The candidate container can see the model path but cannot read all files.
- An aggregate result references evidence from a different source revision,
  image digest, model hash, request, or run.

## Requirements

### Functional Requirements

- **FR-001**: Every validation case MUST emit a versioned fidelity record.
- **FR-002**: A fidelity record MUST identify its tier, exact command, source
  revision, real components, simulated components, network mode, container
  mode, model identity, hardware profile, and whether skipping is failure.
- **FR-003**: Model identity MUST contain a stable model name and immutable
  content revision or digest suitable for rejecting same-name/different-content
  artifacts.
- **FR-004**: The aggregate gate MUST reject missing, malformed,
  contradictory, stale, cross-run, or cross-revision evidence.
- **FR-005**: A mandatory gate MUST NOT pass when its process is skipped,
  unavailable, timed out, replaced by a lower-fidelity case, or missing its
  success evidence.
- **FR-006**: Legacy unit, static, fixture, startup, and socket checks MUST
  retain their narrower value but MUST NOT satisfy a real-model MiniNDN or
  container requirement.
- **FR-007**: The default local aggregate command MUST include the real minimum
  Qwen3 MiniNDN gate and the candidate-container real-workload gate.
- **FR-008**: The default gate MUST fail before any TigerCluster submission is
  considered permitted when Gate A, B, C, or D is unsatisfied.
- **FR-009**: The MiniNDN gate MUST use one User, a Controller/security carrier,
  and at least three Provider roles in separate network/process contexts.
- **FR-010**: The MiniNDN gate MUST use real minimum-Qwen3 weights; fake logits,
  byte payloads, synthetic model packages, and expected-token-only fixtures
  MUST be declared simulated and cannot satisfy the gate.
- **FR-011**: The model used by the default gate MUST be locally resolvable by
  immutable identity before the network experiment starts.
- **FR-012**: The MiniNDN gate MUST use at least two non-empty real prompts.
- **FR-013**: Each prompt MUST have at least one warmup invocation and at least
  three measured invocations.
- **FR-014**: Each measured invocation MUST retain at least eight generated
  token events unless it fails explicitly; early EOS below the threshold is
  not a passing measurement.
- **FR-015**: Each measured invocation MUST retain the complete decoded answer,
  TTFT, ordered per-token latencies, total latency, generated-token count, and
  tokens/s.
- **FR-016**: The result MUST retain per-request records and distribution
  summaries without discarding failed, timed-out, or incomplete invocations.
- **FR-017**: The execution profile MUST explicitly state CPU or GPU, backend,
  device placement, and fallback count; silent fallback MUST fail.
- **FR-018**: One canonical request identifier MUST be established at the User
  boundary and reused throughout the durable invocation.
- **FR-019**: ACK, Selection, operation-status, intermediate execution, and
  Response admissions MUST verify request identifier, attempt, plan/selection
  identity, and model identity before affecting state.
- **FR-020**: The gate MUST retain an ordered lineage record sufficient to
  prove the bindings in FR-019 for every measured request.
- **FR-021**: Negative coverage MUST prove that mismatched lineage fields are
  rejected and do not complete, renew, or mutate the intended request.
- **FR-022**: The local container gate MUST run the same real-model workload
  and evidence contract used by the MiniNDN gate.
- **FR-023**: The container gate MUST record immutable image identity, exact
  resource limits, exit status, OOM status, mounted model identity, and backend
  placement.
- **FR-024**: Container startup, NFD socket, Compose restart, and `/bin/true`
  checks MUST remain separate lower-tier cases.
- **FR-025**: Each monitored long operation MUST have a renewable idle deadline
  and a non-renewable absolute deadline.
- **FR-026**: Only authenticated, correctly bound, monotonically advancing
  progress MUST renew the idle deadline.
- **FR-027**: Progress MUST bind request identifier, operation identifier,
  provider identity, role, attempt, epoch, sequence, phase, and completed work;
  it MUST include total work when known.
- **FR-028**: Duplicate, reordered, forged, wrong-boundary, non-advancing, and
  post-terminal progress MUST NOT renew deadlines.
- **FR-029**: Idle expiry MUST produce a distinct STALLED terminal reason;
  absolute expiry MUST produce a distinct HARD_TIMEOUT reason.
- **FR-030**: Terminal completion, failure, cancellation, stall, and hard
  timeout MUST obey first-terminal-wins semantics.
- **FR-031**: The deadline implementation MUST support deterministic clock
  injection so all boundary cases can be tested without wall-clock sleeps.
- **FR-032**: Progress reporting MUST remain generic NDNSF collaboration
  infrastructure; Qwen/model-specific interpretation MUST remain in NDNSF-DI.
- **FR-033**: Test and experiment outputs MUST include the exact source
  revision, model identity, command, environment profile, random seed where
  applicable, warmup count, measured count, and all skips/failures.
- **FR-034**: A run MUST publish one machine-readable aggregate verdict and one
  human-readable summary with identical pass/fail accounting.
- **FR-035**: TigerCluster three-node validation MUST remain disabled by the
  default local command and documented as the next external-validity stage
  after all local gates pass.
- **FR-036**: The blocking MiniNDN model gate MUST invoke the real-model
  profile with an explicit topology, immutable Qwen3 model identity, and
  `--require-real-model`; it MUST reject `fake`, synthetic payload, missing
  campaign evidence, unresolved tokenizer state, or a missing model digest.
  The gate MUST also verify that all declared Provider roles were selected and
  that every stage emitted artifact-readiness and execution evidence.
- **FR-037**: Prepared model-stage payloads MUST be installed once in a
  repository-local content-addressed store. Import MUST use same-filesystem
  hard links with no byte-copy fallback, every run MUST retain a relative link
  and bundle manifest, and deleting an originating run MUST NOT invalidate the
  shared bundle. A digest or manifest conflict MUST fail before a real gate.

### Key Entities

- **Fidelity Record**: Versioned declaration of what a case actually executed,
  its identities, environment, real/simulated boundaries, and result.
- **Gate Policy**: Mandatory cases and minimum fidelity allowed to satisfy each
  local validation requirement.
- **Model Identity**: Stable model name plus immutable revision/digest and
  artifact location.
- **Invocation Lineage**: Ordered, authenticated bindings for one request from
  Request through Response.
- **Generation Measurement**: One warmup or measured invocation, including
  answer, ordered token events, timing, placement, and terminal result.
- **Operation Progress**: Authenticated monotonic observation for a bounded
  operation.
- **Deadline State**: Start time, last admitted activity, idle deadline,
  absolute deadline, and first terminal outcome.
- **Aggregate Verdict**: Fail-closed accounting across all mandatory local
  gates.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The default aggregate command contains 100% of Gate A-D mandatory
  cases and returns nonzero for every tested skip, fake substitution, stale
  evidence, and missing-field fixture.
- **SC-002**: One accepted MiniNDN run records at least two prompts, two
  warmups, six measured requests, and at least eight ordered generated-token
  events per measured request.
- **SC-003**: 100% of measured requests retain complete answer, TTFT,
  per-token latency, total latency, generated-token count, and tokens/s fields;
  incomplete requests remain visible as failures.
- **SC-004**: 100% of Request, ACK, Selection, admitted progress,
  intermediate-execution, and Response records for accepted requests match the
  User-established request identifier, attempt, plan identity, and model
  identity.
- **SC-005**: The negative lineage matrix rejects 100% of wrong-request,
  wrong-attempt, wrong-plan, wrong-model, duplicate, reordered, and post-terminal
  events.
- **SC-006**: The candidate-container gate produces the same schema and minimum
  request/token counts as the MiniNDN gate while recording image and resource
  identities.
- **SC-007**: Deterministic deadline tests cover valid advance, duplicate,
  reorder, forgery, no-progress stall, continuous-progress hard cap,
  cancellation race, completion race, and post-terminal input with no
  wall-clock sleep.
- **SC-008**: Valid progress renews the idle deadline in 100% of boundary tests
  but never increases the immutable absolute deadline.
- **SC-009**: No unit, fixture, startup, socket, fake-payload, or one-token
  result can satisfy the real MiniNDN or real-container requirements.
- **SC-010**: The default local gate performs no TigerCluster submission and
  produces an explicit "external validation not yet authorized" result until
  all local requirements pass.
- **SC-011**: Reusing one prepared Qwen stage bundle creates zero duplicate
  payload bytes; its run-local reference remains valid after the originating
  run is removed, and the retained manifest accounts for every file and byte.

## Assumptions

- The minimum supported model is Qwen3-0.6B or a smaller official Qwen3 model
  if one becomes available, always pinned by immutable revision/digest.
- CPU execution is acceptable for the local functional gate when declared
  explicitly; it does not substitute for later GPU evidence.
- The local correctness profile uses two prompts, one warmup and three measured
  requests per prompt. The later TigerCluster profile remains five prompts,
  one warmup and five measurements per prompt, with up to 64 generated tokens.
- Existing generic NDNSF Request/ACK/Selection/Response security and token
  mechanisms are reused rather than creating DI-specific wire messages.
- Existing fake and fixture tests remain valuable lower-tier regressions and
  are relabeled rather than deleted.
- Model acquisition is a separate explicit prerequisite. The default gate
  fails with an actionable reason when the pinned artifact is absent; it does
  not silently download a mutable model.
- This feature does not submit jobs to TigerCluster or claim GPU performance.
- The MiniNDN-first recheck is a prerequisite for any later Docker or
  TigerCluster claim. A passing Gate-B subset is diagnostic evidence only;
  external authorization requires a new full Gate A-D plus container
  aggregate after any harness or source change.
