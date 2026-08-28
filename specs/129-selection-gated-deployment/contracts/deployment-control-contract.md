# Reservation and Targeted Selection Contract

## Contract version

`DIReservationSelectionV1` and `SelectionGatedInputV1` are independently
negotiated and additive. Without the DI capability, positive ACK retains normal
NDNSF application semantics: no reservation, reservation tombstone, or mandatory
negative Selection. Without the input capability, current request-input handling
is unchanged.

| DI reservation | Gated input | Required behavior |
|---|---|---|
| off | off | Existing ACK/Selection and input behavior. |
| off | on | ACK carries `SelectionInputKeyOffer`; only selected recipients get a targeted `SelectionInputKeyGrant`; no reservation, tombstone, mandatory negative Selection, DI plan, or assignment. |
| on | off | Reservation-bearing ACK/decision lifecycle; existing input behavior. |
| on | on | Reservation lifecycle plus selected-recipient input-key grant with all DI bindings. |

`SelectionGatedInputV1` requires the normal ACK/Selection exchange. Combining it
with a Selection-free Targeted fast path fails before REQUEST publication;
Targeted calls without this capability remain unchanged.

## REQUEST to ACK

1. Provider authenticates and authorizes the Requester, service and generic
   request metadata; DI mode additionally validates deployment intent.
2. Only `DIReservationSelectionV1` invokes NDNSF-DI's atomic tentative reserve.
3. DI reserve success produces one positive ACK with signed `ReservationLease`;
   failure produces a negative ACK and no reservation. Input-only ACK semantics
   otherwise remain application-defined.
4. A duplicate DI request/attempt returns the same live logical reservation and
   does not allocate again or extend expiry.
5. A DI ACK does not authorize fetch, verify, load, warm, or execution.
6. When `SelectionGatedInputV1` is enabled, REQUEST carries only
   `EncryptedRequestInput` ciphertext or encrypted-object reference; REQUEST,
   ACK and `NOT_SELECTED` never carry its content key.
7. Each successful candidate ACK for that capability carries a signed
   `SelectionInputKeyOffer` with Provider boot epoch and encryption certificate
   binding. This is not a reservation. ACK policy sees authenticated request
   metadata/ciphertext, not plaintext application input.

## ACK collection closure

`ackDeadline = requestPublishedAt + ackTimeout`. The timeout callback atomically
sets `decisionClosed`. Only authenticated/decrypted positive ACKs completed
before closure are eligible. Valid positive ACK completion afterward triggers
targeted `NOT_SELECTED`; there is no post-timeout drain.

USER retains a bounded tombstone until every possible lease for that attempt
has expired. Invalid, unauthenticated, and negative ACKs receive no decision.

## Provider-targeted Selection

One message addresses one Provider/reservation:

```text
/<requester>/NDNSF/SELECTION/<provider-uri-component>/<service...>/<requestId>/<attempt>
```

Required common fields:

```text
schemaVersion, decision, requester, requestId, attempt,
targetProvider, providerBootEpoch, reservationId, reservationDigest,
decisionSequence, issuedAt, expiresAt, requesterSignature
```

`SELECTED` additionally requires common global plan digest, recipient assignment
digest, and `RecipientEncryptedAssignment`. `NOT_SELECTED` forbids assignment
content. Message-name target, payload target, ACK Provider, certificate,
reservation and boot epoch must agree.

With `SelectionGatedInputV1`, exact-target `SELECTED` carries a
`SelectionInputKeyGrant` wrapping the distinct request-input content key to the
certificate in `SelectionInputKeyOffer`. In input-only mode the grant is a
direct Selection extension and requires no reservation, plan, assignment, or
role. With DI also enabled, it is embedded in the encrypted assignment and all
DI bindings are mandatory. The grant is forbidden for `NOT_SELECTED` and
unauthorized recipients.

## Recipient assignment confidentiality

The USER creates a fresh random content key and nonce for each selected
Provider, encrypts only that Provider's minimum assignment projection using
AEAD, and wraps the content key to the authenticated Provider encryption
certificate bound in its ACK. AAD binds the Selection name and all authority
fields. Signature/identity validation precedes decryption. Cross-recipient,
wrong-name, wrong-certificate, replay and tamper cases fail closed.

Under `SelectionGatedInputV1`, request input uses a separate fresh AEAD key before REQUEST publication. Input
AAD binds requester/request/attempt/input digest or reference/expiry. The
per-recipient input-key grant always binds request, attempt, decision, recipient
and certificate; it additionally binds plan, assignment, role and reservation
when DI is enabled. Input, assignment, status and invocation
token keys are never reused across purposes. An unwrapped input key is never
durably persisted and is erased after the last authorized local consumer
terminates or the grant/attempt expires.

## Provider transition

```text
SELECTED:
  compare exact live tentative reservation
  -> atomically commit before tentative expiry
  -> create bounded committed execution lease
  -> decrypt/validate assignment
  -> prepare locally

NOT_SELECTED:
  compare exact live tentative reservation
  -> release
```

The first valid decision is immutable. Duplicate same-digest decisions return
the prior outcome. Every conflicting decision is rejected regardless of
sequence; stale, old-attempt, wrong-epoch, unknown-reservation, and post-expiry
commit attempts do not mutate or resurrect resources. Cancellation/abort uses
a separate authenticated transition.

## Receipt and retry

Provider returns a signed `SelectionDecisionReceipt` identifying the decision
digest and resulting reservation state. USER retries only the exact target with
a bounded attempt/deadline policy. Retry never extends reservation expiry.
Lease expiry is the final cleanup if USER, decision or receipt is lost.

## Stage execution

There is no complete ReadySet and no global ExecutionActivate authority.

```text
source eligible = selected + locally READY
non-source eligible = selected + locally READY + all valid direct inputs
```

Stage evidence binds request, attempt, plan, edge, role, sequence/chunk, payload
digest and signature. Failure/abort is bounded and idempotent. Partial stage
output cannot be accepted as the final Response.

## Contention retry

When the USER cannot assemble the required role reservations, all obtained
reservations receive `NOT_SELECTED`. USER waits for each release receipt or the
corresponding lease expiry before beginning bounded full-jitter exponential
backoff and the next attempt. Maximum attempts and total deadline are mandatory.
The contract provides probabilistic progress only.

## Traffic bound

For `P` valid positive ACK reservations and `S` selected stages:

- decision messages: one logical decision per `P`, plus bounded retries;
- receipts: one logical receipt per `P`, plus bounded recovery;
- progress transitions: zero unsolicited messages;
- stage data/control: proportional to declared DAG edges, not all-to-all;
- lease expiry requires no network traffic.

## Failure semantics

- Invalid ACK: ignore, no reflection response.
- Late valid positive ACK: targeted `NOT_SELECTED`.
- Lost negative decision: bounded retry, then lease expiry.
- Requester crash: lease expiry releases tentative/abandoned reservations.
- Provider restart: boot epoch fences old decisions and local recovery releases
  or expires old resources.
- Assignment decryption/binding failure: fail closed, release/abort matching
  reservation, never execute.
- Downstream failure: propagate abort; upstream work may have occurred, but no
  incomplete pipeline result is accepted.
