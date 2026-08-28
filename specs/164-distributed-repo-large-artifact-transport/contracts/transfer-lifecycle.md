# Contract: Transfer and Lifecycle

## BeginStore

The publisher performs one normal NDNSF collaboration request containing the
ArtifactReference/descriptor and requested durability. Repository ACK metadata
advertises capacity, format/algorithm support, limits, load, and relevant
policy state. ACK metadata is advisory: it does not reserve bytes, pin storage,
or lock repository resources. After ACK_CLOSED, commit_plan selects exact
Providers. Selection carries one authenticated store-task assignment per
replica, and the Provider places the accepted assignment on its bounded
execution queue.

The application does not preselect hidden control mode or issue one
collaboration request per chunk.

## Queue and Transfer-Session Ownership

The queue is the admission/backpressure mechanism. A full or unavailable
Provider returns ACK=false; a selected task that can no longer execute fails
explicitly instead of relying on stale ACK capacity.

When a queued task begins execution, the repository MAY create a private,
durable transfer-session ownership record that binds:

- operation and artifact identity;
- repository identity;
- accepted format, algorithms, geometry, and naming scope;
- expiry/recovery identity when resumable progress is enabled;
- policy epoch.

This record protects partial writes, finalization, and GC correctness. It is
not returned in ACK, does not reserve declared artifact bytes, and does not
serve as the replica-selection input. Database/file critical sections remain
short transactional consistency mechanisms, not long-lived resource locks.

## Data Transfer

The publisher serves root manifest, manifest pages, and artifact Data under the
lease-bound naming scope. Each repository:

- maintains a bounded adaptive Interest window;
- accepts out-of-order Data;
- suppresses duplicates;
- retransmits missing ranges;
- applies backpressure to verification and persistence;
- persists only verified resumable progress;
- keeps memory bounded by in-flight work.

## Resume

Resume requires exact equality of operation-compatible ArtifactReference,
manifest root, geometry, and policy context. The repository reports verified
chunks/ranges; the publisher serves only missing work plus bounded recovery
traffic. Mixed-version or changed-identity partial state is rejected.

## Commit

After all chunks and the full digest verify, a replica:

1. records recoverable finalization intent;
2. atomically finalizes payload bytes;
3. commits artifact metadata;
4. produces an authenticated ReplicaReceipt;
5. makes the artifact eligible for catalog activation.

The user reports requested durability only after retaining the required number
of distinct valid receipts.

## Activation

An artifact becomes `ACTIVE` only after required trust, verification,
persistence, and durability conditions hold. Before activation, normal
discovery cannot return it as reusable content.

## Failure Semantics

- Queue rejection or execution timeout stops new work explicitly. Internal
  transfer-session expiry preserves only policy-valid resume state.
- Cancellation preserves verified progress only when requested and safe.
- Replica failure reports achieved receipts without inventing requested
  durability.
- Digest/trust/manifest failure quarantines or deletes partial state according
  to policy and never activates it.
- Crash recovery completes or rolls back an idempotent finalization journal.
- Garbage collection requires short exclusive metadata ownership and cannot
  race with an active transfer session, finalization, commit, or catalog
  reference.

## Fetch

The consumer resolves an ArtifactReference and capable replicas, validates the
root, retrieves manifest pages/chunks through a bounded window, verifies the
full content identity, and atomically exposes the destination. Multi-source
fetch MAY be added later but must not combine different roots, policy contexts,
or geometries.

## Control-Count Invariant

For fixed replica count and lifecycle outcome, NDNSF control operations are
bounded independently of artifact size, chunk count, and Data-packet count.
Metrics must make violations directly observable.
