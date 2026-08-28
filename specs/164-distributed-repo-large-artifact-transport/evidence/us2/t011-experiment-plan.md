# T011 MiniNDN Recovery Experiment Plan

## Research objective

Determine whether the Spec 164 artifact data plane preserves exact verified
progress and bounded lifecycle state across process interruption, lease expiry,
identity mismatch, concurrency, capacity exhaustion, and partial multi-replica
commit. This is a deterministic functional validation, not a throughput or
latency experiment.

## Frozen subject and environment

- Entry point:
  `Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py --recovery-matrix`
- Canonical implementation:
  `Experiments/NDNSF_DistributedRepo_Recovery_Minindn.py`
- Topology:
  `Experiments/Topology/spec164-artifact-recovery.conf`
- Nodes: one publisher, three repositories, and one consumer
- Links: 1 ms delay and 1 Gbit/s configured capacity
- Artifact: deterministic 65,536-byte payload
- Manifest geometry: four 16,384-byte chunks; 4,096-byte NDN Data payloads
- Trust: one RSA-signed root manifest, digest-bound pages/chunks, and
  repository-specific authenticated receipts
- Scope boundary: collaboration authorization and lease issuance were already
  validated by T007. This experiment begins at the issued, identity-bound
  lease and exercises the network data plane and durable repository lifecycle.

## Hypotheses and acceptance observations

1. **Publisher/repository/consumer interruption**
   - Kill and restart the publisher after repository chunk 0 is durable.
   - Kill and restart the repository after chunk 0 is durable.
   - Kill and restart the consumer after chunk 0 is durable.
   - Expected publication bytes: 16,384 before interruption and 49,152 after
     restart.
   - Expected retrieval bytes: 16,384 before interruption and 49,152 after
     restart.
   - Expected final state: repository `ACTIVE`, consumer destination visible,
     and reconstructed digest equal to the signed artifact identity.

2. **Lease expiry**
   - Expire a lease before bulk work begins.
   - Expected bytes: zero.
   - Expected final state: no active artifact and subsequent work rejected.

3. **Changed-identity resume**
   - Attempt to reuse an operation identifier with a different manifest-root
     identity.
   - Expected bytes: zero additional bytes.
   - Expected final state: exact identity conflict; existing progress remains
     bound to its original identity.

4. **Concurrent sessions**
   - Overlap two operations for the same digest and two operations for
     different digests.
   - Expected same-digest network bytes: one payload only; the second operation
     observes no missing chunks through verified CAS deduplication.
   - Expected different-digest result: independent storage identities and no
     cross-contamination.

5. **Three-replica partial commit and low space**
   - Request three replicas. Let repo1 and repo2 commit while repo3 has less
     configured capacity than the declared artifact.
   - Expected repo3 bytes: zero; expected repo3 state: `FAILED`.
   - Expected durability: requested 3, achieved 2, with exactly two distinct
     retained authenticated receipt identifiers.

## Validity controls

- Every run uses a unique output directory and a watchdog-bounded process.
- Results record transferred payload bytes and terminal state per case.
- Root/page recovery traffic is bounded metadata traffic and is not counted as
  bulk artifact bytes.
- The quick smoke uses a 32 KiB payload and must finish its functionality case
  in under 10 seconds; its timing is not a performance claim.
- The canonical run uses 64 KiB and is accepted only if every case passes.
- Evidence sanitation removes HMAC keys, duplicate payloads, temporary stores,
  ready/stop markers, and consumer output while retaining manifests, public
  key, logs, per-role results, and aggregate summary.
- Failed harness-development attempts are not protocol observations and are
  excluded from canonical evidence.

## Material Passport

- Origin Skill: `experiment-agent`
- Origin Mode: `run`
- Origin Date: 2026-07-30
- Verification Status: `VERIFIED`
- Version Label: `spec164_t011_recovery_v1`
