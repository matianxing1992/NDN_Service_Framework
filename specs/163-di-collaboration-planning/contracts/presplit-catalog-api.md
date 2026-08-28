# Pre-Split Catalog API Contract

## Authority split

Planning receives a read-only, immutable catalog snapshot. Only an
operator-authorized or trusted materialization port can mutate catalog state.

```python
class PreSplitCatalog:
    def get(self, digest: str) -> "PreSplitManifest": ...

    def find(
        self,
        model: "ModelDescriptorRef",
        *,
        backend: str | None = None,
        precision: str | None = None,
    ) -> tuple["PreSplitRef", ...]: ...

    def snapshot(
        self,
        model: "ModelDescriptorRef",
    ) -> "PreSplitCatalogSnapshot": ...


class PreSplitOps:
    def register(self, manifest: "PreSplitManifest") -> "PreSplitRef": ...

    def import_splitter_output(
        self,
        output: "SplitterOutput",
        *,
        model: "ModelDescriptor",
        splitter: "SplitterDescriptor",
    ) -> "PreSplitRef": ...

    def publish_generated(
        self,
        specification: "SplitSpecification",
        *,
        materializer: "SplitMaterializer",
        publisher: "DistributedArtifactPublisher",
        deadline_ms: int,
    ) -> "PreSplitRef": ...

    def retire(self, digest: str, *, reason: str) -> "PreSplitRef": ...

    def revoke(self, digest: str, *, reason: str) -> "PreSplitRef": ...
```

## Registration invariants

- Manifest identity is content-addressed and includes the exact model,
  semantics, graph, splitter, role DAG, fragments, artifacts, runtime
  requirements, and resource requirements.
- Registering the same canonical manifest is idempotent.
- Reusing a human-readable alias for different content is rejected.
- Every artifact has an immutable name/reference, digest, size, producer
  identity, signer/certificate chain, signature algorithm, trust-policy digest,
  creation/expiry metadata, and verification policy.
- A model adapter, RunnerAdapter, or publisher authorized to supply executable
  containers, pickles, custom operators, or native libraries is an explicit
  Provider host-TCB member. Signature/digest validation does not sandbox that
  code; accepting executable content outside this TCB requires a separate
  process/VM sandbox and restricted format policy.
- Registration validates role coverage, graph acyclicity, dependency tensors,
  fragment identity, runtime compatibility, artifact availability, signer
  authorization, and canonical manifest encoding.
- Materialization first writes into a request-scoped, non-executable staging
  location; only a complete size/digest/signature/policy verification may
  publish immutable, content-addressed artifacts and a signed manifest through
  NDNSF-DistributedRepo. The manifest becomes active atomically only after all
  referenced segments are retrievable and verified. Partial or abandoned
  staging state is inaccessible to preparation and is deleted after a bounded
  deadline.
- `publish_generated` is called only by the trusted coordinator after strategy
  output validation. In `DEFERRED` mode this occurs after immutable
  `ACK_CLOSED` and before the same invocation's generic `commit_plan`; any
  failure leaves that invocation uncommitted and produces zero final Selection.
  The strategy never receives this mutation port.
- `retire` prevents new planning but preserves existing evidence.
- `revoke` prevents new planning and new preparation immediately, records the
  signed security or correctness reason and catalog epoch, and notifies active
  attempts through authenticated status. Already running roles follow the
  sealed attempt's explicit abort-or-continue policy; revocation never silently
  changes their authority.
- Catalog mutation never deletes frozen experiment evidence or already
  referenced manifests.

## Snapshot invariants

A `PreSplitCatalogSnapshot` contains only active entries that matched the exact
model query at capture time. It includes:

```text
snapshot ID and epoch
capture and expiry times
model/content/semantics digests
ordered manifest references
catalog state digest
```

Placement strategies cannot call `register`, `retire`, `revoke`, artifact
publication, or arbitrary filesystem methods. They receive only the snapshot.
Catalog snapshots and placement decisions bind the catalog epoch and
trust-policy digest; a stale snapshot cannot authorize a newly revoked
manifest.

## Residency is provider state

Static catalog state answers:

```text
What exact split exists?
What artifacts define it?
What backend and resources does each role require?
```

Validated ACK state answers:

```text
Which provider currently has each fragment?
Is it GPU-loaded, host-resident, disk-resident, or repository-only?
What is the provider boot epoch and estimated preparation time?
What cache epoch and device bind the observation?
Is the entry pinned through Selection expiry, safely reloadable, or evictable?
```

Residency is never persisted as timeless truth in the pre-split manifest.
Each signed residency entry binds the exact manifest/artifact digest,
model name, content/semantics digest, graph range, backend, precision/runtime ABI,
trust-policy digest, size, tier, device when applicable, Provider boot epoch,
cache epoch, observation time, and expiry. GPU/RAM claims become invalid on
Provider restart. Disk claims survive only after re-verification. Selected,
in-flight, or explicitly pinned entries cannot be evicted; eviction of
unpinned entries advances the cache epoch and must be reflected in subsequent
ACKs.

Pre-split entries are reusable products, not a prerequisite for inference. If
the catalog has no feasible exact entry, `PreSplitFirstStrategy` may return a
new `SplitSpecification`; the trusted coordinator materializes and publishes
it, then registers the resulting immutable manifest before final Selection.
