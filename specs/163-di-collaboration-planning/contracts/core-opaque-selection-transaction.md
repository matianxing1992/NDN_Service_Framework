# Generic Core Opaque Selection Transaction Contract

## 1. Purpose

This contract makes one Provider-local Selection decision crash-linearizable
without placing distributed-inference semantics in base NDNSF. It is the
implementable bridge between:

```text
base NDNSF:
  authenticated Selection + one-time ProviderToken + optional opaque lease

application participant:
  opaque application commit state and acceptance payload
```

The canonical implementation uses one Core-owned
`GenericSelectionTxnStore` write-ahead log (WAL). It is a local transaction, not
a cross-Provider transaction, readiness cover, consensus protocol, or
distributed two-phase commit.

This transaction begins only after the requester has committed the complete
generic collaboration plan through
[ndnsf-collaboration-carrier.md](ndnsf-collaboration-carrier.md). The two
commits are intentionally different:

```text
requester commit_plan
  = same-invocation generic plan seal; no token consumption or Provider consent

Provider GenericSelectionTxnStore COMMITTED
  = one Provider's durable final-Selection acceptance and token consumption
```

Neither commit is `PreparationCommit`, and neither creates a global
cross-Provider acceptance barrier.

## 2. Single source of truth and ownership

| Owner | Durable authority |
|---|---|
| Base NDNSF Core | WAL record identity/state; authenticated request, Selection, Provider, boot epoch, payload digest, deadline, ProviderToken disposition, optional opaque lease disposition, participant identity/version, encrypted opaque commit blob/digest, opaque acceptance payload/digest, replay/conflict/tombstone state |
| Registered application participant | Pure validation of opaque payload semantics; canonical application commit blob; opaque acceptance payload; idempotent projection and abort callbacks |
| NDNSF-DI participant | Meaning of GPU admission transition, complete role tuple, encrypted grants, generation fence, DI acceptance evidence, preparation, object, DAG, result, and recovery fields inside its blob |

Core MUST NOT parse the participant blob, profile name, acceptance payload, GPU
units, roles, artifacts, grants, tensors, or recovery fields. It enforces only
generic size limits, exact byte/digest identity, registered participant
identity/version, encryption-at-rest, deadline, token/lease state, and state
transitions.

For a committed Selection, the WAL's encrypted opaque commit blob is the
authoritative logical application state. NDNSF-DI GPU-ledger entries, role
tuples, grants, and generation latches in Python/C++ runtime memory are
idempotent projections of that record, not a second durable source of truth.
No independent Python database may claim a conflicting commit. If a future
participant needs another durable store, that requires a separate transaction
protocol and is outside this contract.

## 3. Generic participant seam

The Core-facing contract is application-independent:

```cpp
struct AuthenticatedSelectionContext {
  GenericTxnId transactionId;
  ndn::Name serviceName;
  ndn::Name requestId;
  uint64_t attempt;
  ndn::Name selectionIdentity;
  Digest selectionPayloadDigest;
  ndn::Name providerIdentity;
  BootEpoch providerBootEpoch;
  MonotonicDeadline localDeadline;
  TokenRecordRef providerTokenRecord;
  std::optional<OpaqueLeaseRecordRef> leaseRecord;
};

struct OpaqueSelectionPrepareResult {
  std::string participantId;
  uint32_t participantVersion;
  ndn::Buffer commitBlob;
  Digest commitBlobDigest;
  ndn::Buffer acceptancePayload;
  Digest acceptancePayloadDigest;
};

class OpaqueSelectionParticipant {
public:
  virtual OpaqueSelectionPrepareResult
  prepare(const AuthenticatedSelectionContext&, span<const uint8_t> payload) = 0;

  virtual void
  onCommitted(const GenericCommittedSelectionView&) = 0;

  virtual void
  onAborted(const GenericTxnId&, GenericAbortReason) = 0;
};
```

The Python binding exposes the same ownership without DI field access in Core:

```python
provider.configure_opaque_selection_store(
    wal_path="/var/lib/ndnsf/provider-id/selection.wal",
    storage_key=provider_secret_32_bytes,
    storage_key_epoch="provider-key-epoch-7",
    max_prepare_ms=1000,
)

provider.register_opaque_selection_participant(
    service_name,
    participant_id="ndnsf-di-v2",
    participant_version=2,
    prepare=di_prepare_selection,       # pure, bounded, no external side effect
    on_committed=di_apply_committed,    # idempotent by transaction_id
    on_aborted=di_discard_prepared,     # idempotent cleanup
)
```

The store must be configured before participant registration. The 32-byte
storage key is supplied by Provider secret management, is never derived from a
wire token, and is never written to the WAL. `max_prepare_ms` is intersected
with the invocation's original monotonic deadline. Identical concurrent
Selection messages join the one in-memory `VALIDATING` owner rather than
starting parallel `prepare()` calls.

`participant_id` selects a registered service-local callback; it is not a Core
DI profile enum. Core passes the authenticated context and exact opaque
Selection payload. It does not pass raw token bytes, input plaintext keys,
mutable lease objects, or device handles.

`prepare()` MUST:

1. be bounded and deterministic for the same authenticated context and bytes;
2. perform all application semantic validation;
3. return a canonical, bounded blob and acceptance payload;
4. have no catalog mutation, artifact fetch/load/warm, GPU allocation,
   externally visible ledger transition, role execution, publication, or
   network side effect.

The NDNSF-DI blob contains the exact logical transition from its ACK offer to
the committed GPU admission, complete role tuple, encrypted grants, generation
fence, and `DISelectionAcceptanceV2` preimage. The blob contains references and
ciphertext, not model weights or plaintext inputs.

`onCommitted()` treats the WAL view as authoritative and installs an
idempotent runtime projection keyed by `transactionId`. It may enqueue
asynchronous preparation only after observing `COMMITTED`. Repeated callbacks
MUST NOT double-allocate capacity, reinstall roles, release grants twice, or
admit execution twice.

When one Provider receives several assignments in the same sealed
collaboration plan, Core carries them in the bounded
`OpaqueAssignmentSetType` container. It preserves item order and exact bytes;
one assignment remains byte-identical for compatibility. Core does not decode
the application meaning of any item.

## 4. WAL record and state machine

The Core WAL has a uniqueness constraint over:

```text
Provider identity / boot epoch / request / attempt / Selection identity
ProviderToken record reference
```

One bounded record contains:

```text
transaction ID and state
service and registered participant ID/version
request / attempt / Selection identity and exact opaque payload digest
Provider identity and boot epoch
ProviderToken record reference and disposition
optional opaque lease record reference and disposition
signed wire deadline and conservative local monotonic expiry
encrypted opaque commit blob, blob digest, and storage-key epoch
opaque acceptance payload and digest
creation / commit / terminal sequence and audit digest
```

The state machine is:

```text
ABSENT
  -> VALIDATING                 in-memory only; no authority change
  -> COMMITTED                  one fsynced WAL transaction: linearization
  -> TOMBSTONED | EXPIRED       retained through the replay horizon

ABSENT | VALIDATING
  -> ABORTED                    no token consumption or application authority
```

The single `COMMITTED` WAL transaction atomically:

1. verifies the still-live generic token, optional lease, deadline, and unique
   Selection identity under the transaction lock;
2. changes the ProviderToken to consumed and the optional lease to committed;
3. installs the encrypted opaque application commit blob as authoritative;
4. installs the opaque acceptance payload/digest as the replayable result;
5. records the exact Selection digest, Provider boot epoch, and tombstone
   horizon.

Token and lease dispositions MUST be replayed from this same WAL transaction;
they cannot be committed earlier in an unrelated store or callback. The fsync
of `COMMITTED` is the only Selection-acceptance linearization point.

After `COMMITTED`, Core invokes `onCommitted()` and exposes the application
acceptance payload through access-controlled, Provider-signed generic status
transport. The callback and receipt publication may occur in either order and
may be retried; neither changes the already committed decision. A role may
prepare or run after its local projection and latches are ready even if the
requester has not yet observed the receipt.

## 5. Crash and conflict recovery

| Cut point | Recovery rule |
|---|---|
| Before or during `prepare()` | No WAL commit, token, lease, DI authority, or preparation side effect exists; retry starts validation again |
| After `prepare()`, before WAL fsync | Discard the returned blob; token and lease remain unconsumed unless their independent deadline expires |
| During WAL append/fsync | A checksum-framed record is either absent or fully `COMMITTED`; torn or unverifiable tails are truncated and never authorize callbacks |
| After `COMMITTED`, before/during `onCommitted()` | WAL replay invokes the callback idempotently when the same boot epoch remains valid; a process restart with a new boot epoch retains historical acceptance/tombstone evidence but fences old roles and reports `PROVIDER_RESTARTED` |
| After projection, before receipt publication | Identical Selection retry/status query returns the stored opaque acceptance payload; no second token or blob commit occurs |
| Receipt lost | Requester remains `UNKNOWN`; identical retry/query recovers the same acceptance |
| Same identity, different payload/blob/acceptance digest | Reject as conflict; never invoke a second participant commit |
| WAL unavailable, storage key unavailable, blob corrupt, or fsync failure | Fail closed before `COMMITTED`; do not consume token/lease or start application work |

An exception from `onCommitted()` after the durable commit does not roll the
Selection decision back. It creates an authenticated application
preparation/projection failure, fences the role closure, and releases resources
according to the committed blob and deadline. This is “accepted, then failed,”
not a contradictory `NOT_SELECTED` decision.

On a Provider process restart, the new non-repeating boot epoch prevents old
role execution. Core preserves the old record long enough to return historical
acceptance plus restart/failure evidence and to reject token replay. It does not
silently activate an old-boot tuple under the new epoch.

## 6. Persistence and confidentiality

- The WAL directory and encryption key are Provider-identity scoped and
  permission-restricted.
- Opaque blobs and acceptance payloads are encrypted at rest; ordinary logs
  contain only transaction IDs, reason codes, sizes, digests, and sequence
  numbers.
- Record and blob sizes are bounded before allocation. Oversized participant
  output aborts before token/lease disposition.
- Storage-key rotation preserves decryptability through the maximum active
  deadline and replay horizon; retirement erases keys only after all dependent
  records are terminal.
- Tombstones remain through the maximum message, token, lease, key-grant,
  status, and clock-skew freshness horizon.
- Only the bound requester or a controller-authorized auditor may retrieve the
  opaque acceptance payload.

## 7. Concurrency and deadline rules

Core serializes transactions by ProviderToken record and Selection identity.
Two requests cannot commit the same token or lease. Duplicate identical
messages join or replay the same transaction; they do not run concurrent
`prepare()` calls after a committed record exists.

Core rechecks the conservative local monotonic deadline immediately before
WAL commit. Time spent in `prepare()` cannot extend it. Participant callbacks,
receipt retransmission, status queries, and cleanup cannot extend any token,
lease, grant, or attempt deadline.

## 8. Migration from the current path

The current `ServiceProvider` flow consumes/deletes ProviderToken pending state
before all DI deployment/assignment validation and dispatch completes. It
cannot serve as the V2 transaction.

Migration order is mandatory:

1. add `GenericSelectionTxnStore`, participant registration, WAL replay, and a
   non-DI opaque participant fixture to base NDNSF;
2. route generic token/optional-lease disposition through the WAL rather than
   the earlier destructive path;
3. implement the NDNSF-DI participant and canonical opaque commit blob under
   `NDNSF-DistributedInference`;
4. enable the V2 DI profile only after crash-cut, conflict, replay, deadline,
   and ownership tests pass;
5. retain the old deployment/READY/activation branch only on the finite frozen
   V1 allowlist until its callers migrate.

Rollback disables the registered V2 participant for new requests, drains or
expires existing WAL records, and preserves tombstones/evidence. It never
reinterprets a V2 blob through the legacy handler.

## 9. Verification obligations

Tests MUST cover:

- a non-DI participant proving the Core seam is application-independent;
- deterministic `prepare()` and rejection of side effects, oversized blobs,
  digest mismatch, callback version mismatch, and unauthorized registration;
- concurrent duplicate and conflicting Selections for one token/lease;
- every crash cut in Section 5, including a torn WAL tail and unavailable
  encryption key;
- idempotent `onCommitted()` projection and callback failure after commit;
- receipt loss, `UNKNOWN`, identical replay, status access control, and
  Provider restart;
- zero token/lease consumption or application side effect before `COMMITTED`;
- exactly one committed opaque blob and acceptance payload per transaction;
- static checks proving Core never parses the NDNSF-DI blob or imports a DI
  schema.
