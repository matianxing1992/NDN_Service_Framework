# Secure Pull-Only Reservation and Stage Status Contract

R1 extends every snapshot binding with reservation ID/digest, Selection
decision digest, global plan digest, assignment digest, and local stage state.
The pull, signature, recipient encryption, monotonicity, and cursor rules below
remain unchanged.

## Capability and name

`SecureSelectionStatusVersion = 1` is negotiated before Selection. The new path
has no plaintext downgrade.

```text
/<provider>/NDNSF/SELECTION-STATUS/<version>/<status-handle>
```

`status-handle` is random and opaque. The name contains no request ID, service,
Selection digest, role, state, or progress value.

## Query contract

Requester sends an exact-name, MustBeFresh, bounded-lifetime signed Interest.
Signed parameters bind:

```text
version, statusHandle, requesterIdentity, requestId, attempt,
queryNonce, issuedAt, expiresAt
```

Provider validates Interest signature/trust schema, original Requester
ownership, handle/attempt/Provider binding, freshness, expiry, and nonce replay
before constructing a response. Invalid queries produce no protected status
payload and increment a bounded diagnostic counter.

## Response contract

Provider serializes one bounded latest snapshot and uses a requester/request-
scoped AES-GCM status epoch key with bounded age/use. The key is wrapped to the
Requester's selected encryption certificate on first use and rotation; every
response uses a unique nonce and bound associated data. Provider signs the
entire Data packet after encryption.

Requester processing order is mandatory:

```text
validate Provider Data signature
-> validate exact name/handle and outer bounds
-> unwrap content key
-> authenticate/decrypt ciphertext
-> validate all inner bindings and expiry
-> enforce monotonically newer sequence
-> expose snapshot
```

Signature, wrapping, AEAD, binding, or sequence failure exposes no status and
cannot advance caller state.

## Polling and events

- A status call is one requester-initiated pull; Provider emits nothing without
  an Interest.
- Automatic polling begins only after Selection when an authorized caller
  explicitly enables it and the final response is pending.
- Interval, backoff, maximum interval, query count, and total time are bounded
  configuration with safe nonzero defaults.
- Polling stops on response, timeout, cancellation, failure, expiry, or any
  terminal state.
- `events(after=cursor)` returns strictly greater sequences. Callers advance
  cursor to the maximum admitted sequence. Expired retained history returns an
  explicit gap indication.

## Negative contract

Wrong requester, wrong recipient key, guessed/replayed handle, unsigned or
stale Interest, tampered signed Data, valid signature with invalid ciphertext,
reused nonce, wrong request/attempt/Selection/Provider/instance/role, stale or
duplicate sequence, oversized reason/details, and plaintext payload all fail
closed and are covered by deterministic and MiniNDN tests.
