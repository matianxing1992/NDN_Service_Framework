# T006 Streaming Artifact Store Evidence

## Result

PASS for the T006 implementation boundary. `artifact-manifest-v2` now has an
independent filesystem content-addressed payload backend in native C++ and
Python. It supports generation-scoped sparse staging files, bounded range I/O,
compact merged verified-range maps, restart-safe partial state, full SHA-256
verification, durable finalization intent, atomic rename into the immutable
digest namespace, existence checks, and safe abort.

Transactional SQLite metadata remains separate from bulk bytes. The artifact
path does not create a database row per small NDN Data packet, while
`exact-packet-v1/sqlite` remains an explicit legacy payload backend.

This evidence is `implemented` and `executed` locally. T007 owns NDN segmented
transfer and collaboration/lease wiring. T008 owns end-to-end activation and
consumer atomic destination. T010 owns the complete cross-domain crash
reconciliation protocol; T006 provides the durable payload-side intent but does
not claim every injected crash point is already reconciled.

## Requirement Traceability

- FR-023: native `PayloadStore` and `MetadataStore` implementations are
  independent and composed only by `RepositoryStoreFacade`; Python exposes
  separate legacy, artifact-v2 payload, and metadata roles.
- FR-024: range write/read, verified progress, flush, finalize, committed
  existence, and abort are implemented and tested.
- FR-025/FR-026 (T006 portion): bulk bytes and compact verified ranges live in
  files; SQLite contains artifact-level lifecycle metadata rather than one row
  per packet. Lease, receipt, catalog, and GC ownership completion remains with
  T009, T010, and T015.
- FR-027: the existing lifecycle journal remains transactional and survives
  reopen.
- FR-028 (T006 portion): partial files remain under `staging/`; only a
  digest-verified atomic rename creates an immutable committed payload. Active
  catalog visibility remains a later lifecycle step.

## Persistence and Resource Matrix

```text
out-of-order range writes                         PASS
overlapping/adjacent verified-range merge         PASS
verified progress survives backend reopen         PASS
configured per-operation I/O bound                PASS
full SHA-256 before committed visibility          PASS
digest corruption                                 REJECTED
incomplete verified coverage                      REJECTED
generation-scoped partial path                     PASS
content-addressed committed path                   PASS
payload fsync + durable finalization intent        PASS
atomic staging-to-committed rename                 PASS
safe abort of partial state                        PASS
empty artifact                                    PASS
one SQLite row per 100 packet-sized writes         NO
legacy exact-packet payload backend preserved      PASS
```

The default one-operation read/write limit is 16 MiB and is configurable
downward. Full digest verification streams with a 256 KiB buffer. The verified
range sidecar is parser-bounded to 1,048,576 canonical ranges and collapses
adjacent or overlapping progress.

## Verification

```text
./waf build --targets=unit-tests,ndnsf-distributed-repo -j2
  PASS

build/unit-tests --run_test=DistributedRepoFilesystemArtifactStore
  4/4 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_streaming_artifact_store.py
  5/5 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_persistence_authority.py
  6/6 PASS

python3 NDNSF-DistributedRepo/pythonWrapper/setup.py build_ext --inplace -j2
  PASS

Regression:
  ArtifactTypes/Manifest/Store native             13/13 PASS
  ArtifactTypes Python                              8/8 PASS
  ArtifactManifest Python                           3/3 PASS
  Exact packets Python                             12/12 PASS
  Tiered cache Python                              11/11 PASS
  Chunked file Python                               2/2 PASS
  HA Python                                        48/48 PASS
  Python bytecode compilation                         PASS
```

## Spec Kit Audit

The deterministic structural audit passed with 44 functional requirements, 12
success criteria, and 20 well-formed tasks. A code-aware T006 review identified
and fixed an initially unbounded single range read/write before closeout.

The overall feature still lacks a global traceability artifact. This local
mapping closes T006 reviewability but does not replace final T019 traceability
and security audit.

## Workflow Gates

- Context Mode: anomaly stats ran; guard health still failed because no project
  ContentDB is bound to `.specify/feature.json`; repository documents and live
  source were authoritative.
- CodeGraph: storage interfaces, persistence authority, orchestration callers,
  new backends, and tests were inspected before and after implementation.
- Spec Kit: active feature, task dependencies, storage contract, requirements,
  checklist, and strict structural audit were verified.
- GSD: installation health is `healthy`; the unrelated phase-34 missing-summary
  item remains informational.
- ARS: not applicable to implementation of a frozen persistence contract; no
  literature, paper, comparative, or statistical claim was introduced.
