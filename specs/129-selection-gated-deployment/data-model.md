# Data Model: Reservation-Bearing ACK and Dependency-Driven Execution

## RequestCapabilities

Fields: bounded canonical set of independently negotiated generic capability
identifiers, including optional `SelectionGatedInputV1` and
`DIReservationSelectionV1`, bound to requester/request/attempt/service.

Rules: carried by generic `RequestMessage`; unknown mandatory capability fails
before publication or at Provider admission. `SelectionGatedInputV1` does not
require `DeploymentIntent`. `DIReservationSelectionV1` does.

## DeploymentIntent

Fields: requester identity, request ID, attempt, service, artifact/model digest
and locator, allowed variants, roles, generic constraints, protected input or
locator, ACK timeout, total deadline, signature.

Validation: bounded canonical encoding; non-empty identity/request/attempt;
authorized artifact reference; deadline after ACK timeout; no embedded model.

## ReservationLease

Exists only for `DIReservationSelectionV1`; an ordinary NDNSF ACK never creates
this object or its state transitions.

Fields: requester, request ID, attempt, Provider, Provider boot epoch,
reservation ID, service, normalized resource commitment, offer digest,
encryption certificate name/digest, created time, expiry, sequence, signature.

Validation: positive ACK only; exact identity binding; future bounded expiry;
certificate belongs to Provider; canonical resource commitment; unique logical
tuple `(requester, request, attempt, provider, bootEpoch, reservationId)`.

State:

```text
TENTATIVE -> COMMITTED -> PREPARING -> READY -> EXECUTING -> COMPLETED -> RELEASED
TENTATIVE -> NOT_SELECTED | CANCELLED | EXPIRED -> RELEASED
COMMITTED/PREPARING/READY/EXECUTING -> FAILED | CANCELLED | EXPIRED -> RELEASED
```

## EncryptedRequestInput

Fields: request ID, attempt, input digest or encrypted-object locator, input
algorithm/version, nonce, ciphertext/tag when inline, associated-data digest,
content-key ID, expiry. The content key itself is absent.

Rules: fresh symmetric key and nonce; AAD binds requester, request, attempt,
service, input digest/reference and expiry; no plaintext fallback; the input
key is distinct from invocation tokens, assignment key and status key.

## SelectionInputKeyOffer

Fields: requester, request ID, attempt, Provider, Provider boot epoch,
encryption certificate name/digest, supported algorithm/version, expiry,
signature.

Rules: generic ACK extension for `SelectionGatedInputV1`; proves no resource
reservation and carries no input key. Provider authenticates/authorizes the
request metadata without receiving plaintext input. Certificate identity,
chain, algorithm and expiry are validated before selection.

## SelectionInputKeyGrant

Fields: input content-key ID, recipient Provider/certificate digest, wrapped
input key, request/attempt/Selection digest, expiry and AAD digest; optional DI
role, plan, assignment and reservation digests.

Rules: present only inside exact-target `SELECTED` recipient material for a
recipient explicitly authorized to consume original input; valid without DI
fields when only `SelectionGatedInputV1` is negotiated; DI fields are mandatory
and exactly bound when both capabilities are negotiated. Forbidden in REQUEST,
ACK, `NOT_SELECTED`, and unauthorized/downstream-only projections. The unwrapped key is
never durably persisted and is erased after the last authorized local consumer
terminates or the grant/attempt expires.

## SelectionDecision

Fields: schema version, decision (`SELECTED` or `NOT_SELECTED`), requester,
request ID, attempt, target Provider, Provider boot epoch, reservation ID and
digest, service, decision sequence, issued time, expiry, global plan digest,
assignment digest/envelope when selected, Requester signature.

Rules: exactly one target and reservation; `NOT_SELECTED` has no assignment;
`SELECTED` requires plan and encrypted assignment; the first valid decision is
immutable; a same-digest duplicate is idempotent; every conflicting decision
fails closed regardless of sequence. Cancellation/abort is a separate message
and transition. Commit requires a still-live tentative lease and creates a
bounded committed execution lease; expiry cannot be reversed.

## SelectionDecisionReceipt

Fields: decision digest, reservation ID, Provider/boot epoch, accepted state,
reason, sequence, signature.

Purpose: confirm commit or release. Missing receipt triggers bounded exact-
target retry but cannot extend the reservation lease.

## SelectionDecisionTombstone

Fields: request ID, attempt, closed-at time, selected reservation IDs, observed
positive reservations, final plan digest, retain-until time.

Rules: created only for `DIReservationSelectionV1`; immutable selected set after closure; retained at least until the latest
possible reservation expiry; late valid positive ACK maps to `NOT_SELECTED`.

## GlobalExecutionPlan

Fields: requester, request ID, attempt, artifact digests, canonical role DAG,
terminal output role, selected Provider/reservation commitments, data naming
rules, total deadline, plan digest, signature.

Rules: acyclic; every required role assigned; every assignment references a
live positive ACK; digest computed before projections; immutable per attempt.

## ProviderAssignmentProjection

Fields: global plan digest, target Provider/boot epoch, reservation, local
roles, artifact/model fragments, local execution target, direct predecessor and
successor data names/identities, local constraints, assignment digest.

Rule: contains no unrelated Provider assignment. Multiple local roles may be
represented in one projection when they share one Provider/reservation.

## RecipientEncryptedAssignment

Fields: recipient identity and certificate digest, key algorithm, wrapped
content key, nonce, ciphertext, authentication tag, associated-data digest.

Rules: fresh key/nonce; AEAD AAD binds Selection name, decision, request,
attempt, Provider, boot epoch, reservation, plan and assignment; no plaintext
fallback.

When both capabilities are enabled, the assignment may contain a
`SelectionInputKeyGrant`; the assignment content key and input content key
remain distinct. With input-only capability, the grant is a direct targeted
Selection extension and no assignment envelope is required.

## DeploymentInstance

Fields: instance ID, reservation/plan/assignment digests, Provider/boot epoch,
role, artifact, execution target, state, local resource lock, sequence, expiry,
reason.

Rules: prepare only after valid selected decision; resource lock persists until
local role terminal/abort; warm reuse requires exact immutable bindings.

## StageInputEvidence

Fields: request, attempt, plan digest, producer Provider/role, consumer role,
sequence/chunk, payload digest/reference, produced time, signature.

Rules: direct edge must exist in DAG; exact current attempt/plan; monotonic or
declared chunk identity; replay and wrong-edge material rejected.

## StageAbort

Fields: request, attempt, plan digest, source role/Provider, affected roles,
reason, sequence, deadline, signature.

Rules: idempotent; propagates only along plan dependencies; prevents final
partial-result acceptance and releases remaining local locks.

## EncryptedStatusSnapshot and RequestEventCursor

Status binds reservation, decision, plan, assignment, instance, role, stage,
state, progress, reason and monotonic sequence. It remains Provider-signed and
Requester-encrypted. Cursor presents only `sequence > cursor`, reports history
gaps, and stops at terminal outcome.

## Persisted Migration

R0 `DeploymentPlan`, `DeploymentInstance`, `ProviderReadyMessage`, `ReadySet`
and `ExecutionActivateMessage` records may be read only to fail closed or
release orphan resources. R1 authority comes exclusively from ReservationLease,
SelectionDecision, encrypted assignment, local preparation and stage evidence.
Any compatibility reader is versioned, observable, non-authoritative and
removed after the documented compatibility gate.
