# T005 Artifact Manifest v2 Evidence

## Result

PASS for the T005 implementation boundary. The canonical signed-root codec,
content-addressed page hierarchy, derived naming, chunk/full digest checks,
algorithm negotiation, policy epoch, revocation evaluation, resource bounds,
and fail-closed negative cases are implemented and exercised through native and
Python APIs.

This is `implemented` and `executed` as local contract/unit evidence. It is not
yet `wired` into the segmented transfer/runtime path; T007 owns that integration.
No MiniNDN throughput or end-to-end activation claim is made by T005.

## Requirement Traceability

| Requirement | Implementation | Test evidence |
|---|---|---|
| FR-013/FR-014 | canonical signed root plus resolved NDNSF trust context | valid signature, invalid signature/key/publisher, expiry, epoch, revocation |
| FR-015/FR-016 | bounded content-addressed page graph and derived names | page/chunk graph, empty artifact, missing/duplicate/deep/name-scope negatives |
| FR-017 | chunk and full-object digest verification before later activation | chunk/full success, corruption, truncation |
| FR-018 | capability- and policy-intersected algorithms | RSA-SHA256, ECDSA-SHA256, Ed25519 positives; downgrade negative |
| FR-019 | bounded codecs, graph counts, depth, entries, signature, crypto work | truncation, extension, entry bomb, depth and crypto-budget negatives |
| FR-020 | critical field, substitution, downgrade, mixed-resume rejection | native and Python negative matrices |

## Security Matrix

```text
publisher signature valid                         PASS
RSA-SHA256 / ECDSA-SHA256 / Ed25519              PASS
one asymmetric verification per root graph       PASS
page digest recomputation                         PASS
chunk payload digest                              PASS
full artifact digest                              PASS
artifact/name/geometry substitution               REJECTED
capability or version downgrade                   REJECTED
revoked/expired/wrong publisher trust context     REJECTED
unknown critical extension                        REJECTED
missing/duplicate/too-deep graph                  REJECTED
truncated/extended/entry-bomb encoding            REJECTED
cryptographic-work budget exceeded                REJECTED
mixed policy/manifest resume identity             REJECTED
```

## Verification

```text
./waf build --targets=unit-tests,ndnsf-distributed-repo -j2
  PASS

build/unit-tests --run_test=DistributedRepoArtifactManifest
  8/8 PASS

python3 setup.py build_ext --inplace
  PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_artifact_manifest.py
  3/3 PASS

Regression:
  ArtifactTypes Python                         8/8 PASS
  OperationMetrics Python                     5/5 PASS
  Persistence authority Python                6/6 PASS
  Frozen legacy subject Python                3/3 PASS
  Exact packets Python                       12/12 PASS
  Tiered cache Python                        11/11 PASS
  Chunked file Python                         2/2 PASS
  HA Python                                  48/48 PASS
  ArtifactTypes/OperationMetrics/Store native 7/7 PASS
```

## Spec Kit Audit

Structural scan: PASS (44 functional requirements, 12 success criteria, 20
tasks). Post-implementation T005 verdict: PASS with no CRITICAL or HIGH
finding.

One overall-feature MEDIUM limitation remains: the structural scanner reports
no global traceability artifact. The local FR-013 through FR-020 mapping above
closes T005 reviewability but does not replace project-wide traceability.

The trust verifier consumes a resolved NDNSF trust-policy context. Certificate
discovery and construction of that context from the live NDNSF validator are
intentionally not claimed here; T007 must wire it without allowing wire input
to self-assert trust.

## Workflow Gates

- Context Mode: stats ran; guard health failed because no project ContentDB was
  bound to `.specify/feature.json`; repository documents were authoritative.
- CodeGraph: T002 types and current callers were inspected before implementation;
  the new verifier, codecs, tests, and Python boundary were re-indexed.
- Spec Kit: prerequisites passed, checklist was 16/16 complete, structure audit
  passed, and T005 remained outside T006 persistence/T007 transfer ownership.
- GSD: installation health validation is recorded at closeout.
- ARS: not applicable to implementation of an already frozen trust contract;
  no literature, paper, or statistical claim was added.
