# Implementation Plan: Reservation-Bearing ACK and Dependency-Driven Execution

**Branch**: `Experimental` | **Date**: 2026-07-21 | **Revision**: R1 |
**Spec**: [spec.md](spec.md)

## Summary

Replace Spec 129 R0's advisory ACK and complete READY-set activation barrier.
For R1, NDNSF-DI atomically creates a bounded tentative reservation before a
positive ACK. At the existing ACK timeout, the Core USER closes the candidate
set and sends one Provider-targeted decision for every R1 reservation-bearing positive ACK. A selected
Provider receives a hybrid-encrypted minimum assignment projection; an
unselected or late Provider receives `NOT_SELECTED` and releases the exact
reservation. Lease expiry remains the final cleanup mechanism.

Selected roles prepare locally. Source stages run when locally READY;
non-source stages run when locally READY and their authenticated direct inputs
arrive. The R0 ReadySet/ExecutionActivate authority is migrated out of the
maintained path. NDNSF changes remain generic; GPU/model/DAG/backoff policy
stays in NDNSF-DI.

The independent `SelectionGatedInputV1` capability protects application input
with a fresh symmetric content key before discovery. No candidate receives that
key in REQUEST, ACK, or `NOT_SELECTED`. Each `SELECTED` message wraps the input
key only to selected recipients authorized for original-input access. This is a
generic opt-in NDNSF security primitive, not a DI reservation consequence.

## Technical Context

**Language/Version**: C++17 Core/runtime; Python 3 bindings and NDNSF-DI

**Primary Dependencies**: ndn-cxx Face/Data/Interest/Validator/KeyChain,
NDN-SVS publication transport, existing NAC-ABE and `HybridMessageCrypto`,
Boost.Test, pybind11, MiniNDN/NFD

**Storage**: Bounded durable/in-memory reservation and decision-tombstone state
plus existing DI runtime journals; no new database

**Testing**: C++ and Python contract tests, negative crypto/security tests,
restart/concurrency tests, existing generic regressions, and one fresh frozen
MiniNDN matrix

**Performance Goals**: Prompt release of unused reservations; no global READY
barrier; measurable pipeline overlap; bounded decision/retry/status traffic;
report reservation amplification and backoff rather than hiding it

**Constraints**: Preserve V2 security/token invariants; one ACK timeout window;
no post-timeout validation drain; no plaintext exact assignment; no UAV/codec/
model-family/workload branch; no Spec 128 evidence change; no R0 evidence reuse

## Constitution Check

### Pre-design gate

| Principle | Result | Plan response |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Extends V2 REQUEST/ACK/Selection/Response with versioned capability; no generated or split-name path. |
| Security Is Part Of Data Path | PASS | Keeps NAC-ABE, permission, token, replay, Provider permission, certificate validation, signature, and AEAD checks. |
| CodeGraph First | PASS | Current compact Selection, group encryption, ACK timeout, Provider filtering, and R0 tests were inspected. |
| Spec-Driven Durable Work | PASS | R1 supersedes contradictory R0 contracts before code changes. |
| Verify With Right Scope | PASS | Unit, security, restart, concurrency, compatibility, and fresh MiniNDN gates are required. |
| Cohesive Outcome-Based Tasks | PASS | Each task closes a behavioral migration with tests and evidence; no file-operation fragments. |

### Post-design gate

PASS for planning. Security, failure cleanup, mixed-version fencing, rollback,
task cohesion, and fresh evidence paths are specified. Implementation remains
blocked until the pre-implementation audit passes.

## Architecture and Ownership

| Concern | Owner | Existing seam | R1 responsibility |
|---|---|---|---|
| Generic decision wire and validation | NDNSF Core | `NDNSFMessages.{hpp,cpp}` | Versioned reservation identity, `SELECTED/NOT_SELECTED`, recipient envelope references, receipts, bounds, and strict decode. |
| Generic capability negotiation | NDNSF Core | `RequestMessage`, request builders/parsers | Carry both independent identifiers outside DI `DeploymentIntent`; reject unsupported mandatory combinations before authority changes. |
| Provider-targeted Selection naming/transport | NDNSF Core | `utils.{hpp,cpp}`, `ServiceUser`, `ServiceProvider` | Include exact target and attempt, publish one decision per Provider, parse/fence exact recipient, bounded receipt/retry. |
| ACK window and late dispatch | NDNSF user runtime | `PendingServiceCall`, ACK callbacks and scheduler | For negotiated DI reservations, atomically close at existing timeout, snapshot verified candidates, retain tombstone, and send a negative decision for a late reservation-bearing ACK. |
| Recipient confidentiality | NDNSF crypto | `HybridMessageCrypto`, certificate helpers | Fresh AEAD key per assignment, wrap to ACK-bound recipient certificate, bind all authority fields in AAD. |
| Request input confidentiality | NDNSF Core | request payload/reference, ACK key offer and targeted Selection grant | Under `SelectionGatedInputV1`, encrypt input once before REQUEST, collect a signed certificate offer without reservation semantics, and wrap its key only to selected authorized recipients; DI bindings are conditional. |
| Local reservation and quota policy | NDNSF-DI Core/provider | `core/{contracts,state,eligibility}.py`, provider/runtime journal | Atomic tentative reserve, quotas, idempotency, expiry, commit/release, counters and reasons. |
| Global plan and private projection | NDNSF-DI planner/client | `plan.py`, `planner/`, `client.py` | Canonical DAG commitment and minimum per-recipient assignment projection. |
| Preparation and stage eligibility | NDNSF-DI provider/adapters | Provider and RunnerAdapter seams | Verify/load/warm under committed lease; source/direct-dependency eligibility; hard pin until role completion/abort. |
| Abort and retry | NDNSF-DI client/provider | request lifecycle and journals | DAG failure propagation, bounded randomized exponential backoff, attempt fencing, explicit probabilistic-liveness metrics. |
| Binding/status | Binding + existing owners | pybind11, `pythonWrapper/ndnsf`, secure status path | R1 parity; retain pull-only encrypted status and cursor rules. |
| Evidence | tests/Experiments | Spec 129 runner | Frozen fresh matrix, exact-once cells, message/reservation/release/retry/overlap attribution. |

## Protocol Flow

```text
USER                               Candidate Provider
 | REQUEST(intent, attempt, ackTimeout)       |
 |------------------------------------------->|
 |                                atomic tentative reserve
 |<-- positive ACK(ReservationLease, cert) ---|
 |                                             |
 | ACK timeout: decisionClosed = true          |
 | resolve GlobalExecutionPlan                 |
 |                                             |
 |-- targeted SELECTED + encrypted projection ->| selected
 |-- targeted NOT_SELECTED(reservation) ------>| unselected
 |                                             |
 |<-- DecisionReceipt -------------------------|
 |                                             |
 | late positive ACK after closure             |
 |-- targeted NOT_SELECTED(reservation) ------>|
```

When `SelectionGatedInputV1` is negotiated, REQUEST carries
`EncryptedRequestInput`, never plaintext input or its content key. Candidate ACK
carries `SelectionInputKeyOffer`; `SELECTED` may carry
`SelectionInputKeyGrant`; REQUEST, ACK and `NOT_SELECTED` never carry the key.
Without that capability, existing request-input behavior is unchanged.

```text
Selected pipeline

Stage A: SELECTED -> verify/load/warm -> READY -> execute -> Data(A)
                                                        |
Stage B: SELECTED -> verify/load/warm -> READY ----------+-> execute -> Data(B)
                                                                       |
Stage C: SELECTED -> verify/load/warm -> READY -------------------------+-> execute

No complete-set READY aggregation and no global ExecutionActivate authority.
```

### ACK deadline and decision closure

1. `ackDeadline = requestPublishedAt + ackTimeout`.
2. ACK eligibility requires authentication and decryption completion before the
   timeout callback atomically sets `decisionClosed=true`.
3. No post-timeout drain extends the window.
4. After closure, a valid positive ACK is late and receives `NOT_SELECTED`.
5. A bounded tombstone persists until all possible reservation leases expire.

### Reservation lifecycle

```text
FREE -> TENTATIVE_RESERVED -> COMMITTED -> PREPARING -> READY
                                                READY -> EXECUTING -> COMPLETED -> RELEASED
TENTATIVE_RESERVED -> NOT_SELECTED | CANCELLED | EXPIRED -> RELEASED
COMMITTED/PREPARING/READY/EXECUTING -> FAILED | CANCELLED | EXPIRED -> RELEASED
```

Only `DIReservationSelectionV1` gives ACK this meaning. The Provider authenticates
and authorizes the Requester/service before reserving. Reserve precedes positive
ACK, and ACK does not authorize fetch/load/warm. Selection must atomically commit
before tentative expiry and create a bounded committed execution lease; an
expired reservation is never resurrected. `NOT_SELECTED` accelerates release;
lease expiry is mandatory because decisions and Requesters can disappear.

### Per-recipient Selection

The canonical name is Provider-targeted and attempt-scoped, for example:

```text
/<requester>/NDNSF/SELECTION/<provider-uri-component>/<service...>/<requestId>/<attempt>
```

For DI, each message binds one reservation. `SELECTED` includes a common plan digest
and recipient assignment digest. `NOT_SELECTED` includes no assignment. The
exact assignment is AES-GCM encrypted with a fresh content key wrapped to the
authenticated encryption certificate advertised by the ACK. The Provider name
may remain visible in the NDN name; content confidentiality is mandatory.

Under `SelectionGatedInputV1`, application input is independently AEAD-encrypted before REQUEST publication.
The input ciphertext or encrypted-object reference binds request/attempt and
input digest. Selection reuses neither invocation tokens nor the assignment
key: it wraps the distinct input content key to each selected authorized
certificate advertised by `SelectionInputKeyOffer`. Input-only grants bind no
DI object; when DI is also enabled, AAD additionally binds plan, assignment,
role, reservation and expiry.

### Dependency-driven execution

The global plan is a canonical DAG commitment. Each Provider receives only its
role, resources, artifact, direct predecessor/successor data names, and local
constraints. A source runs at local READY. Other roles run after local READY
and valid direct inputs. Stage data carries request/attempt/plan/role/sequence/
digest/signature. Failure propagates abort; only the terminal output role can
produce an accepted final Response.

### Contention and liveness

Cross-Provider atomic acquisition and a global scheduling controller are not
introduced. Partial acquisition can occur. Every tentative reservation is
leased; failed acquisition retries after full-jitter exponential backoff with
bounded attempts and total deadline. This gives probabilistic progress only.
Before retry, USER sends `NOT_SELECTED` for every reservation acquired by the
failed attempt and waits for its receipt or lease expiry; only then does the
randomized backoff begin. Metrics must expose collision, lease hold, release, backoff, exhaustion, and
starvation observations; documentation must not claim deterministic fairness.

## Security Design

- Validate the ACK signature, Provider identity, boot epoch, offer and
  encryption certificate before constructing a decision.
- Sign every decision and bind request/attempt/target/reservation/sequence.
- Use fresh per-assignment AEAD keys and nonces; wrap the key to the exact
  recipient certificate; bind name and all authority fields in AAD.
- Under `SelectionGatedInputV1`, use a distinct fresh input content key before
  REQUEST; expose only ciphertext
  or encrypted-object reference, and grant that key only in selected,
  input-authorized recipient projections. Never persist an unwrapped input key;
  erase it after the last authorized local consumer terminates or the grant expires.
- Apply only the first valid Selection decision to an exact live reservation.
  Same-digest duplicates are idempotent; every conflicting later decision is
  rejected regardless of sequence. Cancellation/abort is a separate transition.
- Retain one-time Provider token and provider permission checks; reservation is
  capacity commitment, not an authorization bypass.
- Invalid/unauthenticated ACKs receive no response to avoid reflection.
- Enforce identity/service/global tentative quotas and prevent duplicate
  REQUEST from extending leases.
- Preserve pull-only requester-confidential status.

## Compatibility, Migration, and Rollback

1. Negotiate `DIReservationSelectionV1` and `SelectionGatedInputV1`
   independently. Calls without the DI capability retain current ACK semantics
   and never acquire DI reservations; calls without the input capability retain
   current input handling.
   Reject `SelectionGatedInputV1` on the Selection-free Targeted fast path before
   publishing REQUEST; do not alter ordinary Targeted calls.
2. Introduce R1 contracts/readers before switching writers.
3. Implement targeted per-Provider decisions alongside R0 compact Selection
   only for negotiated R1; instrument both paths.
4. Migrate NDNSF-DI from advisory `ProviderCapabilityOffer` to
   `ReservationLease`, and from ReadySet/ExecutionActivate to local DAG gates.
5. After R1 focused gates pass, remove maintained R1 callers of R0 activation;
   keep any compatibility reader non-authoritative and bounded.
6. Rollback may disable either capability independently. Disabling the DI
   capability restores the pre-existing ACK path; all R1 reservations expire
   without requiring an R0 reader to execute them.
7. Persist schema version, attempt, boot epoch, reservation and decision state;
   malformed or mixed authority fails closed rather than guessing.

## Validation Strategy

### Deterministic gates

- Wire round-trip/bounds/version/malformed tests for lease, decision, receipt,
  encrypted projection, stage evidence, abort, and tombstone.
- Atomic reserve/ACK, negative ACK, duplicate REQUEST, quota, expiry, restart,
  release-cause, authorization-before-reserve, non-DI positive-ACK compatibility,
  and journal tests.
- Exact-target naming, one-message-per-reservation, decision-close race, late
  ACK, tombstone retention, lost receipt, duplicate/reorder, conflicting-decision,
  expired-commit, and old-attempt tests.
- Cross-recipient decrypt, wrong certificate/AAD/name/nonce/signature/replay,
  plaintext input/assignment scan, missing/extra input-key grant, key erasure/no
  plaintext-key persistence,
  permission/token and Provider identity negatives.
- Three-stage DAG eligibility, overlap, stale input, failure propagation,
  cancellation, terminal output and resource-pin tests.
- Full-jitter deterministic-seed bounds, exhaustion, total deadline, and
  explicit no-fairness-claim tests.
- Existing HELLO, selective ACK, NAC-ABE, permission, token, Targeted and
  generic service regressions.

### Fresh MiniNDN confirmation

Freeze twelve unique cells before execution:

| Cell | Frozen scenario ID | Required evidence |
|---|---|---|
| 1 | `baseline-three-provider` | three reservation-bearing ACKs, one commit/execution, two negative decisions, three terminal releases |
| 2 | `input-only` | encrypted input selection succeeds with zero DI reservation/plan/assignment/role/negative-decision requirement |
| 3 | `lost-negative-decision` | bounded exact-target recovery or lease-expiry fallback; no orphan |
| 4 | `lost-decision-receipt` | same-digest retry is bounded and the decision remains idempotent |
| 5 | `stale-conflicting-decision` | stale/reordered/conflicting decision is rejected without changing the first transition |
| 6 | `input-tamper` | tampered/cross-recipient input or assignment decrypt succeeds zero times; packet plaintext scan is zero |
| 7 | `provider-restart` | boot-epoch mismatch is fenced and stale reservation state is reclaimed |
| 8 | `partial-reservation-contention` | partial reservations close before bounded randomized retry; no carryover |
| 9 | `dependency-branch-overlap` | direct-predecessor gates permit measured branch overlap and use zero global activation |
| 10 | `contention-retry-exhaustion` | maximum attempts/total deadline terminate with an exhaustion counter and no orphan |
| 11 | `authorized-status-cursor` | authenticated pull returns strictly newer cursor state with bounded query count |
| 12 | `adversarial-status` | unauthorized, replayed, stale, or malformed status is rejected with zero plaintext fallback |

Every cell is once-only with no automatic rerun. Report Payload, decision,
receipt, retry, timeout, Nack, reservation hold/release cause, stage overlap,
completion and latency. Preserve failed cells and Spec 128 hashes.

## Project Structure

### Documentation

```text
specs/129-selection-gated-deployment/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/
│   ├── deployment-control-contract.md
│   └── secure-status-contract.md
├── quickstart.md
├── tasks.md
└── traceability.md
```

### Source Code

```text
ndn-service-framework/
├── NDNSFMessages.{hpp,cpp}
├── utils.{hpp,cpp}
├── HybridMessageCrypto.{hpp,cpp}
└── Service{User,Provider}.{hpp,cpp}

NDNSF-DistributedInference/
├── ndnsf_distributed_inference/core/
├── ndnsf_distributed_inference/{client,provider,plan,deployment}.py
├── ndnsf_distributed_inference/app_sdk/
└── cpp/ndnsf-di/

pythonWrapper/
tests/
Experiments/
```

## Complexity Tracking

| Complexity | Why required | Simpler alternative rejected |
|---|---|---|
| One targeted decision per DI reservation ACK | Releases real tentative reservations and protects exact assignment | Group compact Selection leaks assignments and cannot explicitly close every reservation. |
| Decision tombstone | Late ACK can arrive after normal pending state deletion | Relying only on lease delays recovery and loses decision auditability. |
| Recipient hybrid encryption | Service-group NAC-ABE lets other authorized Providers decrypt group Selection | Plain Provider entry filtering occurs after disclosure. |
| Lease plus probabilistic retry | Avoids introducing global controller/consensus while bounding partial acquisition | Random retry without lease can hold resources indefinitely. |
| Local DAG gates | Enables pipeline overlap and removes complete-set barrier | Global activation delays independent/source stages. |
