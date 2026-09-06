# Spec 164 Final Code-Aware Audit

## Verdict

**PASS for Spec 164.**

**TigerCluster NDNSF-DI + Qwen external-validity work is permitted**, subject
to Specs 162/163's own frozen configuration, security, resource, correctness,
and evidence gates.

This verdict is limited to Spec 164's admitted artifact-transport evidence. It
does not mean that the default local aggregate suite proves real distributed
Qwen deployment. See
[Local Test and Deployment Fidelity Audit](local-test-deployment-fidelity-audit.md),
whose cross-module verdict is **BLOCK** until the real-model MiniNDN, local
Docker, request-ID continuity, and activity-deadline gates are added.

The default store-control lifecycle remains:

```text
Request
→ advisory ACK offers
→ ACK_CLOSED
→ commit_plan with exact assignments
→ Provider bounded task queue
→ QUEUED → RECEIVING → VERIFIED → COMMITTED → ACTIVE
→ Response
```

ACK does not reserve bytes, lock GPU/storage capacity, or create a lease. The
Provider performs execution-time admission when its queued task runs.

## Mandatory gate execution

| Gate | Result | Verified boundary |
|---|---|---|
| Context Mode | PASS | The real prompt `CTX_HOST_ACCEPT_20260730_SPEC164_82c5e90bd113` was captured as a correct-project `user-prompt`; final `health` passed with five fresh file-backed sources and seven trusted hooks. The guard now distinguishes host-owned `hooks.json` freshness from Codex's normal runtime rewrites of `config.toml`. |
| CodeGraph | PASS | Current producer registration, bounded adaptive fetch, Collaboration assignment, queued lifecycle, manifest trust, persistence, and callers were inspected from indexed source. |
| Spec Kit | PASS | Spec, plan, tasks, contracts, traceability, evidence, migration, security, and post-hoc amendment boundary agree. |
| GSD | PASS | The resumable multi-phase campaign sequence and negative-evidence retention were preserved. |
| Academic Research Suite | PASS WITH CAUTION CLOSED | The 11-fallacy audit rejected pooled/survivor-only reinterpretation. The ≥64 MiB amendment was declared post-hoc and required a new confirmatory campaign. |

## Controlling evidence

Canonical campaign:

```text
path: results/spec164-artifact-confirmatory-campaign-20260730T1030Z
campaignId: spec164-artifact-20260730T101414Z
manifest SHA-256:
  0fbcd3d4f77dbadba5e90eaad3a23d8425f9a67e0fbd9fca401cc38aff568996
candidate/admitted/excluded cells: 96 / 24 / 72
warmups: 24 retained, 24 PASS
measured: 120 retained, 120 PASS
measured repetitions per admitted cell: exactly 5
unique/missing run IDs: 144 / 0
independent CSV/Decimal verification: PASS
```

The third campaign remains immutable negative evidence:

```text
results/spec164-artifact-stability-campaign-20260730T0935Z
measured: 117 PASS / 3 FAIL
original SC-003 verdict: FAIL
```

Those failures were all first CanBePrefix Interest timeouts at r1/c16. The
initial adaptive Interest previously bypassed the existing retry budget.
T030 put it under the same bounded retry semantics and exact retransmission
accounting. A real r1/c16 MiniNDN diagnostic then completed all 16 cold
destinations while recording recovered timeouts.

The post-third-campaign SC-003 clarification is prospective, not retroactive:
large-artifact throughput means 64 MiB and above, matching this feature's
scope and SC-002. One MiB cells remain mandatory diagnostics and were all
retained in the confirmatory campaign.

## Success criteria

| Criterion | Verdict | Confirmatory evidence |
|---|---|---|
| SC-002 digest/raw ≥0.85 at ≥64 MiB | PASS | Median `0.975401`; bootstrap 95% CI `[0.925489, 1.015141]`; completion PASS |
| SC-003 signed/digest ≥0.90 at ≥64 MiB | PASS | Median `0.996644`; bootstrap 95% CI `[0.984451, 1.069018]`; completion, point estimate, and lower-bound gates PASS |
| SC-004 bounded control operations | PASS | Public Collaboration smoke: 2 control operations, 16 Data segments, 1 selected replica, 3 lifecycle phases |
| SC-007 read ≤1.20x, write ≤1.50x | PASS | Completion, read-amplification, and write-amplification gates PASS with fresh-destination cold retrieval |
| SC-011 complete formal cells | PASS | All 144 scheduled run IDs retained and unique; five measured runs per admitted cell |
| SC-012 MiniNDN before TigerCluster | PASS | Local correctness/security/recovery/performance evidence precedes cluster work |

## Architecture, ownership, and security

- NDNSF owns generic Request/ACK/ACK_CLOSED/Selection/Response, permissions,
  tokens, replay protection, and opaque exact-assignment delivery.
- NDNSF-DistributedRepo owns artifact identity, capability negotiation,
  signed-root trust, segmented NDN Data, bounded retry/backpressure, queue
  admission, verification, persistence, receipt, activation, resume,
  recovery, migration, rollback, and GC.
- NDNSF-DI consumes the public artifact API; repository logic is not moved
  into NDNSF Core.
- Signed roots bind artifact identity, algorithms, geometry, policy epoch,
  page/chunk/full digests, and critical extensions. Parsing and allocation are
  bounded before use.
- Corrupt, partial, substituted, downgraded, revoked, stale-generation, or
  mixed-resume content cannot become active.
- HMAC receipts remain authorization-domain evidence, not public
  non-repudiation.
- No debug authorization bypass or ACK-time reservation path was found in the
  active queued store flow.

## Verification

```text
Spec 164 Python discovery: 102/102 PASS
native Python extension rebuild: PASS
failure-focused producer registration regression: PASS
first-Interest r1/c16 real-seam diagnostic: PASS
confirmatory campaign independent verification: PASS
```

The original `pytest` attempt was not a test failure; system Python has no
pytest module. The repository's native `unittest` discovery completed.

## Residual limitations

1. The largest admitted local cell is 64 MiB r1/c1. One GiB and 16 GiB cells
   were excluded by frozen host resource gates.
2. The confirmatory campaign did not repeat the predecessor's physical
   ceiling; 659.613 Mbit/s remains historical environment evidence.
3. Five repetitions and deterministic bootstrap intervals support an
   engineering acceptance gate, not a broad population-level paper claim.
4. SQLite remains the embedded metadata authority; cluster-scale writer
   contention is not yet established.
5. Legacy reservation rows and lifecycle spellings remain decode-only
   migration input. The active API, service-name map, capability calculation,
   and store path contain no capacity-reservation operation; see
   `evidence/remediation/t033-remove-capacity-reservation.md`.
6. Context Mode host capture is unavailable; repository artifacts are the
   authoritative checkpoint.

## TigerCluster permit boundary

The prior Spec 164 technical block is closed. TigerCluster may now be used for
Specs 162/163 NDNSF-DI + Qwen external-validity experiments. This permit does
not pre-approve a result: OCI/SIF identity, model/tokenizer hashes, graph and
partition identities, GPU/node bindings, artifact publication/fetch,
distributed execution, complete answers, TTFT, per-token latency, total
latency, tokens/s, failures, and immutable evidence must still satisfy those
Specs' own gates.
