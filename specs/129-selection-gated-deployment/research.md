# Research: Reservation-Bearing ACK and Dependency-Driven Execution

## Decision 1: Positive ACK is a bounded tentative reservation

**Decision**: Only a request explicitly negotiating
`DIReservationSelectionV1` makes NDNSF-DI atomically reserve bounded local
capacity before a positive ACK. The Provider authenticates and authorizes the
Requester/service first. Negative ACK reserves nothing. Ordinary NDNSF positive
ACKs retain their existing meaning. ACK alone does not authorize
fetch, verify, load, warm, or inference.

**Rationale**: Advisory availability can disappear between ACK and Selection.
A signed lease makes a positive ACK redeemable while keeping deployment work
behind Selection.

**Alternatives considered**: R0 advisory ACK (rejected: selection race);
deployment during ACK (rejected: candidate amplification and wasted loading).

## Decision 2: The existing ACK timeout is the sole decision window

**Decision**: Eligibility closes atomically when the configured timeout callback
runs. ACK authentication and decryption must complete before closure. No
post-timeout drain extends the window.

**Rationale**: This is deterministic and keeps latency bounded. A late valid
positive ACK receives `NOT_SELECTED` instead of entering the plan.

**Alternatives considered**: Observation-time eligibility plus unbounded crypto
drain (rejected: indeterminate decision latency); second grace window (rejected:
duplicate timing contract).

## Decision 3: Send one decision to every positive-ACK reservation

**Decision**: For `DIReservationSelectionV1`, the USER sends exact-target
`SELECTED` or `NOT_SELECTED` to each valid reservation-bearing positive ACK. A
bounded tombstone handles late ACKs. Lease expiry is the
final release guarantee.

**Rationale**: Message absence cannot distinguish loss, delay, crash, and
non-selection. Explicit negative decisions release scarce capacity promptly.

**Alternatives considered**: Notify selected Providers only (rejected: unused
capacity remains locked until expiry); new negative-message family (rejected:
one versioned decision enum is simpler).

## Decision 4: Selection becomes Provider-targeted and receiver-confidential

**Decision**: One targeted Selection name/message binds one Provider,
request/attempt, reservation and boot epoch. `SELECTED` carries a fresh
hybrid-encrypted minimum assignment projection. All projections share a common
global plan digest.

**Rationale**: Current compact group Selection is encrypted for a service
attribute and filters Provider entries only after group decryption. It does not
protect exact assignments from other authorized Providers.

**Alternatives considered**: One compact ciphertext with all entries (rejected:
over-disclosure); direct public-key encryption of the entire assignment
(rejected: size and algorithm limits); hide target in opaque name (deferred:
content secrecy is the present requirement and name metadata remains explicit).

## Decision 5: Reservation ID remains distinct from request ID

**Decision**: Every decision binds both request ID and reservation ID plus
attempt, target Provider and boot epoch.

**Rationale**: One request can have many Provider reservations and later retry
attempts. A delayed negative decision must not release a newer reservation.

**Alternatives considered**: Request ID alone (rejected under retry, restart,
multi-Provider and message-reorder behavior).

## Decision 6: Replace global ReadySet activation with local DAG eligibility

**Decision**: A source stage executes when selected and locally READY. Other
stages execute when locally READY and all authenticated direct predecessor data
arrive. No complete-set READY aggregation authorizes execution.

**Rationale**: Global activation delays independent stages and prevents
pipeline overlap. Direct dependencies are the actual execution prerequisites.

**Alternatives considered**: R0 USER-coordinated barrier (rejected by design
feedback); Provider all-to-all readiness (rejected: quadratic traffic and still
stronger than DAG dependency); execute at Selection (rejected: local runtime may
not be prepared).

## Decision 7: Use leases and bounded randomized retry, with limited claims

**Decision**: Partial acquisition is recovered by first sending `NOT_SELECTED`
for every acquired reservation, waiting for each receipt or lease expiry, and
then applying full-jitter exponential backoff, attempt bound and total deadline.

**Rationale**: This keeps the simplified system controller-free while avoiding
permanent resource hold. It provides probabilistic progress, not deterministic
fairness or starvation freedom.

**Alternatives considered**: Global controller/consensus (rejected for this
revision's simplification goal); cross-Provider atomic commit (rejected:
blocking/failure complexity); fixed retry delay (rejected: synchronized retry).

## Decision 8: Keep policy in NDNSF-DI and transport/security in NDNSF

**Decision**: NDNSF owns reusable, opt-in targeted Selection transport, closure
and late dispatch, decision contracts, recipient encryption, receipts and fencing.
NDNSF-DI owns resource meanings, reservation policy, quotas, plan projections,
DAG gates, model lifecycle, backoff and abort.

**Rationale**: Positive ACK cannot globally mean GPU reservation for all NDNSF
applications. Only reusable message/security machinery belongs in Core.

**Alternatives considered**: Encode GPU/DAG fields in NDNSF (rejected:
application-policy leakage); implement separate DI transport (rejected:
duplicates generic security and loss handling).

## Decision 9: R0 evidence is migration evidence, not R1 acceptance

**Decision**: Previously completed R0 T001--T005 are reopened/replaced. R1 must
receive fresh deterministic and MiniNDN evidence.

**Rationale**: R0 tests explicitly assert zero ACK reservation and complete-set
activation, the opposite of R1.

**Alternatives considered**: Preserve old checkmarks (rejected: false completion
claim); create Spec 130 (rejected because the user explicitly requested revision
of still-open Spec 129 and no R1 campaign has run).

## Decision 10: Offer selection-gated input as an independent NDNSF capability

**Decision**: `SelectionGatedInputV1` is a generic, independently negotiated
NDNSF capability. USER encrypts application input once with a fresh symmetric
key before REQUEST. REQUEST carries only ciphertext or an authenticated encrypted
object reference. Each candidate ACK carries a signed generic
`SelectionInputKeyOffer` that implies no reservation. Exact-target `SELECTED`
wraps the key to that certificate only for an authorized recipient. The grant
works without DI fields; plan/assignment/role/reservation bindings become
mandatory only when both capabilities are negotiated. The key never appears
in REQUEST, ACK, or `NOT_SELECTED`, is
never persisted unwrapped, and is erased at last authorized-consumer termination
or grant/attempt expiry.

**Rationale**: Candidate Providers must inspect intent and reserve capacity but
must not learn inference input before being selected. Separating input and
assignment keys limits access and avoids repeated bulk encryption.

**Alternatives considered**: Service-group encryption in REQUEST (rejected:
all authorized candidates can decrypt); delay publishing input until Selection
(rejected: changes request/reference availability and duplicates transfer);
grant the key to every selected role (rejected: violates least privilege).

The Selection-free Targeted fast path cannot provide post-Selection disclosure.
R1 rejects that combination before REQUEST publication and leaves ordinary
Targeted behavior unchanged; any Targeted-specific key bootstrap is future work.

## Decision 11: The first valid Selection decision is immutable

**Decision**: The first valid `SELECTED` or `NOT_SELECTED` for an exact
reservation is final. A same-digest duplicate is idempotent. A conflicting
decision is rejected regardless of sequence. Cancellation and abort use
separate authenticated state transitions. A selected commit must occur before
tentative expiry and creates a bounded committed execution lease.

**Rationale**: Allowing a later sequence to reverse a resource decision can
release an executing reservation or resurrect an expired one.

## Verified Current-Code Facts

- `ServiceUser::PublishServiceSelectionMessageV2` accepts a Provider name but
  publishes the compact Selection name without a target Provider component.
- The multi-provider path builds one `ServiceSelectionMessage` with multiple
  Provider entries.
- `PublishMessage` treats REQUEST and SELECTION as group control messages and
  attaches group NAC-ABE key wrapping.
- Providers decrypt the group Selection and only afterward reject a message
  without a local Provider entry.
- R0 tests assert ACK mutation count is zero and require a complete ReadySet
  before `ExecutionActivateMessage`; both are deliberate R1 migration targets.
