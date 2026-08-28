# T008 Trusted Single-Replica End-to-End Evidence

## Verdict

PASS. The single-replica path now composes the T005 authenticated manifest
graph, a freshly revalidated T007 upload lease, T006 streaming CAS/lifecycle
persistence, an authenticated replica receipt, transactional activation, the
T007 adaptive segmented data plane, and an atomic consumer destination.

## Implemented Closure

1. `ArtifactReplicaSession` requires an `ArtifactUploadLease` and revalidates
   expiry, capacity, operation, repository, and exact artifact identity at the
   data-plane entry.
2. The session verifies the RSA-authenticated root and complete page/chunk graph
   before accepting payload work.
3. Chunk bytes are digest-verified before their ranges become durable verified
   progress.
4. The full CAS digest is streamed and atomically finalized while the durable
   lifecycle journal remains the recovery authority.
5. Receipt retention, `VERIFIED → COMMITTED → ACTIVE`, and the active catalog
   entry are one SQLite transaction. A retry is byte-identical and idempotent.
6. The receipt binds repository, operation, policy epoch, storage generation,
   and exact ArtifactReference. HMAC-SHA256 authentication is verified before
   persistence and again by the consumer.
7. Normal lookup returns only `ACTIVE` catalog entries.
8. `AtomicArtifactDestination` accepts bounded out-of-order ranges, streams the
   full digest, and uses a no-overwrite hard-link commit so incomplete or
   conflicting bytes never replace the destination.
9. Native `DataPacket` now exposes immutable Data content in addition to the
   exact wire packet, allowing the callback-streaming fetcher to persist bytes
   without assembling the artifact inside the fetcher.

## Deterministic Local Evidence

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec164_artifact_lifecycle.py
  5/5 PASS
```

The cases cover:

- out-of-order two-segment delivery followed by chunk verification;
- full lifecycle, receipt authentication, active lookup, and atomic retrieval;
- corruption rejection with no committed payload/catalog/destination;
- receipt tamper and operation-substitution rejection;
- recovery from durable `VERIFIED` state after payload finalization;
- lease expiry revalidation at the control/data boundary.

## MiniNDN Evidence

Canonical retained run:

```text
specs/164-distributed-repo-large-artifact-transport/evidence/us1/
  minindn-functional-20260730T031203Z/
```

Frozen topology:

```text
publisher --1 ms, 1 Gbit/s-- repo --1 ms, 1 Gbit/s-- consumer
```

Observed functional results:

```text
overall verdict                         PASS
payload size                            65,536 bytes
success publication segments           16
success publication Interests          16
success publication retransmissions    0
success lifecycle                       RESERVED, RECEIVING, VERIFIED,
                                        COMMITTED, ACTIVE
success receipt authenticated           true
success consumer destination visible    true
success consumer content digest         d8f32a3b4c195f65444e342b0c61bdaf
                                        114802e84ab7821a1f88e3f66ea15100
success root asymmetric verifications   1
corruption status                       CORRUPTION_REJECTED
corruption active                       false
corruption destination visible          false
```

The corruption run changes one transmitted payload bit after freezing the same
trusted manifest. It is rejected by the chunk digest before activation.

The run is explicitly marked `performanceClaim=false`. Its approximately
5.8-second and 5.7-second scenario durations are orchestration observations,
not throughput results. Functional smoke mode separately passed in under ten
seconds with `SPEC164_ARTIFACT_MININDN_SMOKE_OK`.

Fixture receipt keys, duplicate payload bytes, repository runtime state, and
consumer output bytes were removed after verdict capture. Public trust
material, summaries, and role logs remain. The prior superseded matrix was
moved to a recoverable `/tmp/spec164-superseded-evidence-*` directory.

## Regression Evidence

```text
Spec 164 manifest Python                  3/3 PASS
Spec 164 streaming store Python          5/5 PASS
Spec 164 adaptive transfer Python        5/5 PASS
Exact-packet compatibility Python       12/12 PASS
Native ArtifactManifest                  8/8 PASS
Native ArtifactTransfer                  5/5 PASS
Native FilesystemArtifactStore           4/4 PASS
Core Python extension resource build     PASS (-O0 -g0, -j1)
Spec Kit strict structural audit         PASS (no blockers or warnings)
```

## Security Boundary

The HMAC receipt is intended for an already authorized NDNSF control domain:
the key must be provisioned through protected configuration or an NDNSF
protected response and is never carried in a receipt or catalog. It provides
efficient repository authentication to authorized verifiers but not public
non-repudiation. The publisher root remains RSA-authenticated. The MiniNDN
fixture uses a test-only 256-bit key and deletes it after the run.

T007 owns the actual `begin_collaboration → ACK_CLOSED → commit_plan` adapter.
T008 consumes the exact resulting `ArtifactUploadLease` contract and rejects
unbound or expired leases; the functional MiniNDN harness starts at that
lease-bound data-plane boundary rather than duplicating a second control
protocol.

## Five-Tool Gate

- Context Mode: statistics collected; repository guard again failed closed
  because the project ContentDB is absent. Repository state was authoritative.
- CodeGraph: verified the live content callback, lifecycle call graph, tests,
  and blast radius after implementation.
- Spec Kit: strict audit passed with no blockers or warnings.
- GSD: health was previously verified healthy; unrelated phase-34 information
  remains unchanged.
- ARS: experiment-agent plan/run discipline produced the Material Passport,
  matched functional scenarios, timeout monitoring, explicit claim boundary,
  and sanitized evidence.

## Harness Corrections Preserved as Lessons

Before the canonical run, four non-protocol harness defects were found and
fixed: MiniNDN global-argv capture, wrong static-route argument type/IP,
relative role-script path, and a logical-name/naming-template scope mismatch.
The final matrix was rerun from scratch after lease enforcement; no failed run
was reclassified or used as positive evidence.
