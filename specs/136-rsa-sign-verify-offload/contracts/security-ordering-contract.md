# RSA And Single-Worker Ownership Contract

## Runtime Configuration

The one binary exposes a default-off publication preparation setting:

```text
publicationPreparationWorkers = 0  -> face-inline-rsa
publicationPreparationWorkers = 1  -> worker-rsa
publicationPreparationQueueCapacity = fixed positive bound
```

Values greater than 1 are rejected for Spec 136. Existing callers retain
worker count 0 and existing `publishAsync()` behavior.

## Thread Ownership

| Operation | Required owner |
|---|---|
| Generate publication deadlines | Independent APP pacer in both modes |
| Call `publishAsync()` in control | Face/io_context after one APP post |
| Call byte `publishAsync()` in treatment | APP pacer directly |
| Admission and sequence reservation | Serialized by the caller path and Core/SVS locks |
| Copy publication input | Before worker admission |
| Build/encode/sign publication Data | Face at 0; one FIFO worker at 1 |
| Mapping, DataStore, SVS state, advertisement | Face/io_context |
| Face I/O and application callbacks | Face/io_context |

The worker cannot access `Face`, mutate SVS state, insert mapping/store data,
advertise a sequence, or invoke application callbacks. Prepared results return
through one Face post. One FIFO worker preserves preparation order; a
multi-worker reorder gate is outside R2.

The R2 off-Face guarantee is limited to the byte-oriented worker-backed
`publishAsync()` overload. Name-only and packet overloads remain outside this
claim. Every cell records Face, APP-pacer, and publication-call thread identity
and rejects a caller-path mismatch.

## Admission, Failure, And Shutdown

- Queue capacity is checked before sequence reservation.
- Rejection reserves no sequence and has an explicit reason.
- Each accepted publication has exactly one terminal result: committed, failed,
  or cancelled.
- A signing/encoding failure after reservation is explicit and prevents later
  commit for that producer; the sequence is not skipped or reused.
- Queue saturation cannot fall back to inline preparation.
- Shutdown stops admission, drains or explicitly cancels accepted jobs, joins
  the worker, and leaves no unaccounted or advertised-but-uncommitted sequence.
- Detached workers, unbounded retry, silent drop, and callback-after-destruction
  are forbidden.

## RSA Contract

- Every peer owns a persistent RSA-2048 identity.
- Publication Data and Sync Interests use
  `SignatureSha256WithRsa`; SHA256 digest signing and HMAC Sync Interests are
  not admissible substitutes.
- Each peer installs the other's certificate and performs real signature
  verification through the configured validators.
- Signed Sync Interests are validated before state processing.
- Fetched outer Data is validated, then encapsulated inner Data is validated
  before subscriber delivery.
- `maxPiggyDataSize=1` forces this fetched path in formal cells; fixing or
  measuring piggyback validation is outside R1.
- Preflight rejects one tampered Data and one tampered signed Interest and
  proves neither reaches application/protocol processing.

## Required Focused Tests

- default worker count preserves current inline behavior;
- treatment creates exactly one FIFO worker;
- queue full rejects before reservation with no inline fallback;
- ordered commit/delivery under delayed worker completion;
- signing failure and shutdown account for every submission once;
- Face/state/store operations never execute on the worker;
- valid RSA Data/Interest pass and tampered objects fail;
- same binary selects both modes with no other runtime difference.
