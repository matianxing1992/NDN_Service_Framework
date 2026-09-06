# Spec180 Candidate Input Integrity Check

**Date**: 2026-09-03  
**Scope**: temporary YOLO canonical packages inspected before T011-A

## Finding

The temporary package manifests under `/tmp/spec180-yolo-*` are not a current
sealed Spec180 candidate. Most shared candidates advertise `DetectHead0` and
`DetectHead1`; one inspected package (`/tmp/spec180-yolo-oo1dxoc3`) uses the
current `DetectShard0` and `DetectShard1` names but has no trusted catalogue
signature. The current exporter, YOLO adapter, case policy, and runner
contract require both the current role names and a registered signature.

## Decision

The packages are rejected before MiniNDN startup. A manifest-only rename or
adding a detached signature is not permitted because the candidate digest and
graph/role identity would no longer describe one signed package. T011-A must
regenerate a package with the current exporter, then verify the graph digest,
external initializer digest, candidate catalogue signature, and adapter
semantic partition as one package identity.

This is an input-integrity blocker, not a model-performance result and not a
reason to reopen the frozen Spec175 baseline.
