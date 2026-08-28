# Selection-Gated Boundary Repair Contract

## Supersession

`DIConflictAdmissionV1`, `ConflictAdmissionCoordinator`, global conflict
ordering, authority epochs and centralized-versus-lease-only modes are not part
of Spec 130. No maintained API, runner option, manifest field or evidence claim
may depend on them.

## Generic NDNSF Contract

NDNSF provides application-neutral secure lifecycle primitives:

```text
REQUEST -> zero or more authenticated ACKs -> per-target SELECTION -> RESPONSE
```

An application may register an opaque ACK-liability policy with these conceptual
operations:

```text
classify_ack(attempt, provider, authenticated_ack)
  -> no_liability | liability(key, horizon, opaque_context)

build_terminal_decision(liability, selected)
  -> opaque_decision_payload

on_terminal_receipt(liability, authenticated_receipt)
  -> accepted | rejected
```

The planned application-neutral C++/binding surface is:

```text
AckLiabilityContext
  requester, service, requestId, attempt, provider, providerBootEpoch,
  authenticatedAck, ackDigest, windowClosed

AckLiabilityPolicyResult
  requiresTerminalDecision, liabilityKey, expiresAtMs,
  opaqueApplicationContext

AckLiabilityPolicyHandler(AckLiabilityContext)
  -> AckLiabilityPolicyResult

TargetedDecisionBuilder(AckLiabilityContext,
                        AckLiabilityPolicyResult,
                        selected)
  -> opaqueApplicationDecision

TargetedDecisionReceiptHandler(liabilityKey, authenticatedReceipt)
  -> accepted | rejected
```

Names may be adjusted mechanically during implementation, but the field and
ownership contract may not: Core owns only the identity/delivery fields and
opaque bytes; the application owns every byte of application context/decision.
The existing generic ACK payload and Selection assignment/recipient-encryption
envelopes are reused where their current security contract fits. No
`DIConflictAdmissionV1` or other new DI wire mode is introduced.

Core owns authenticated delivery, exact-target naming, bounded retransmission,
receipt correlation and tombstone retention. It does not inspect application
capability names or opaque context fields. The public C++/Python hook names must
remain generic and cannot mention DI, reservation, GPU, model, role or DAG.

After ACK-window closure:

1. Core authenticates a late ACK against the AttemptKey.
2. The application policy classifies it.
3. A new liability always receives `selected=false`; no candidate or winner is
   added and no response callback is reopened.
4. Core publishes the exact-target decision and tracks an idempotent receipt.
5. Tombstone state is retained until every liability closes or its bounded
   horizon expires.

Tombstone capacity is reserved before REQUEST publication and bounded by the
authorized Provider set, per-request Provider limit, per-identity quota and
global quota. Exhaustion rejects a new request before it can create remote
liability; it never evicts an active liability.

## NDNSF-DI Reservation Contract

NDNSF-DI interprets `DIReservationSelectionV1` and supplies the liability
adapter. For this capability only:

```text
Provider authenticates/authorizes REQUEST
  -> Provider atomically reserves local finite resource
  -> positive ACK carries signed bounded reservation evidence
  -> Requester selects or rejects every positive reservation
  -> SELECTED commits; NOT_SELECTED promptly releases
```

A generic/non-DI positive ACK has no reservation meaning.

The exact reservation decision binds requester, request ID, attempt, service,
Provider identity/boot epoch, reservation ID/digest, decision sequence and
expiry. `SELECTED` additionally binds the NDNSF-DI plan and receiver-specific
encrypted assignment. `NOT_SELECTED` carries no input key or assignment.

## Two-Requester Contention and Retry Contract

Providers make no global ordering decision. Each Provider atomically rejects a
claim that conflicts with its live local reservation/pin. Each Requester closes
only its own attempt.

Before retry:

```text
all positive ACK liabilities receive NOT_SELECTED
  -> each release receipt accepted OR authenticated finite expiry reached
  -> no live reservation remains for the attempt
  -> sample full-jitter delay from production entropy
  -> create a fresh attempt with fresh security/application bindings
```

Full jitter is `Uniform(0, min(cap, base * 2^(attempt-1)))`, truncated by the
remaining absolute deadline. Maximum attempts and deadline both apply. Repeated
collision may exhaust; no fairness guarantee is made.

## Execution Pin Contract

`SELECTED` commits a reservation but execution begins only after application
plan/assignment validation and local preparation. When a worker becomes able to
use the resource, NDNSF-DI records an `EXECUTING` pin.

- Completion releases after output/terminal state is durably attributed.
- Cancellation/deadline/renewal failure first enters `STOPPING`, fences the
  worker, confirms local stop, then releases.
- Timer expiry cannot directly free an `EXECUTING` pin.
- Provider restart must fence the old boot executor before reusing the binding.
- A stale completion/stop event cannot release another attempt's pin.

## Dependency Contract

NDNSF-DI validates an acyclic plan and distributes only the minimum encrypted
assignment needed by each selected Provider. Eligibility is:

```text
source role:
  selected + pin committed + local preparation complete

downstream role:
  source conditions + every authenticated direct-predecessor StageDataEvidence
```

Stage data binds attempt, plan, producer/consumer Provider and role, chunk,
sequence and payload digest. Independent ready branches may overlap. Join roles
wait for their declared predecessor set. Missing/tampered/stale data never
becomes eligible and causes bounded descendant abort/release.

## Confidentiality and Integrity

- REQUEST input stays independently AEAD-encrypted before publication.
- Input key appears only in a selected recipient's public-key-wrapped grant.
- Exact assignment appears only in recipient-encrypted Selection context.
- ACK and NOT_SELECTED contain neither input key nor exact assignment.
- All request, ACK, decision, receipt, stage and response transitions preserve
  signer/identity, request-attempt, token, replay and digest checks.
- Logs, journals, runner metadata and analysis must not contain plaintext input,
  plaintext exact assignment or content keys.

## Real Fault Evidence Contract

Each formal cell must prove:

1. distinct MiniNDN host, identity, process and NFD mapping for every required
   actor;
2. one invocation and immutable source/manifest hashes;
3. observed real fault action/effect at the Provider, link/routing,
   production-payload or process boundary;
4. event-derived message/reservation/pin/retry/stage/terminal metrics;
5. no assigned expected counters, local deterministic substitute or hidden
   automatic retry;
6. retained negative and harness-invalid outcomes;
7. unchanged frozen Spec 129 hashes before and after.
