# Spec 170 TigerCluster D2b negative probes — 2026-08-18

This is pre-freeze diagnostic evidence for the r22 SIF.  It does not close
T033 or authorize T029.

## Runs

| Case | Job | Nodes | Result | Evidence |
|---|---:|---|---|---|
| peer mismatch | 201008 | itiger08/09 | PASS | `negative-peer-mismatch.pass`; user request failed with `ACK_CLOSED does not cover every selected Provider`; no executed response marker |
| request replay | 201009 | itiger08/09 | PASS with qualification | first fixed request ID executed; second use of the same ID failed with `ACK_CLOSED does not cover every selected Provider`; no executed response marker, but the Provider log did not emit an explicit replay-rejection reason |
| missing segment, original 1.5 s ACK window | 201010 | itiger08/09 | FAIL/diagnostic | request ended at ACK closure before a fault marker; no partial-output response |
| missing segment, 5 s ACK window | 201011 | itiger08/09 | FAIL/diagnostic | same ACK-closure outcome; the faulting Provider was READY but did not enter the selected execution set |
| missing segment, 15 s ACK window | 201012 | itiger08/09 | FAIL/diagnostic | same outcome; gate ended `SPEC170_D2B_CASE_WAIT_FAIL`, with no `fault-missing-segment.injected` marker |

All jobs passed the exact-SIF staging/build-record checks and used runtime SIF
SHA-256 `50478785fc9f7ef836f087e3f14954bba4982fb4196d3a6a20868c334f55eef0`.
The first peer-mismatch run on itiger07 is excluded from protocol evidence
because that node failed Provider readiness before the request; it remains a
node-specific infrastructure diagnostic.

## Interpretation

Peer mismatch and request-level replay now have Tiger behavior evidence, but
the replay result needs a direct rejection marker before it can be called a
complete replay-proof row.  The missing-segment case is not yet a valid
partial-output test: the fault-provider process reaches `READY`, while the
user closes ACK collection without selecting it, so the dependency-fetch
checkpoint is never reached.  Extending the ACK window alone did not change
that behavior.  Do not report this as proof of `NDNSF_DATA_V1` partial-output
recovery or rejection.

## Required correction

Add a fault-provider readiness/ACK smoke gate (or a deterministic selected
Provider assignment) before the missing-segment case.  Require an explicit
`NDNSF_DI_EXPERIMENT_FAULT_INJECTED type=missing-segment` marker and a
terminal no-response/no-partial-output marker.  If the fault-provider path
cannot produce an ACK in isolation, fix that integration path before rerunning
 the full partial-output campaign.

## Follow-up diagnostic correction (local, not yet Tiger-validated)

The workload had treated `NDNSF_DI_NATIVE_PROVIDER_READY` (model/runtime
materialization) as sufficient to release the User. Provider permission
installation is asynchronous, so that barrier could race the first ACK. The
workload now requires both the READY marker and the target-service
`Installed provider permission` log before creating its provider-ready marker.
The corrected D2 bundle was built and validated locally as
`r22-d2-bundles-v23-permission-barrier`, bound to the unchanged r22 SIF
SHA-256 `50478785fc9f7ef836f087e3f14954bba4982fb4196d3a6a20868c334f55eef0`.
This is a diagnostic fix only; it is not a Tiger PASS until a healthy-node
partial run reaches ACK, Selection, fault injection, and terminal no-partial
output evidence.
