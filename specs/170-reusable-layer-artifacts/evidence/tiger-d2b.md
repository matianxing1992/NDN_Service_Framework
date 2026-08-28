# Spec 170 TigerCluster D2b — cross-Provider GPU qualification

## Run identity

- Job: `200985`
- Result: `COMPLETED`, elapsed `00:01:54`, exit `0:0`
- Nodes: `itiger08` and `itiger09`, one allocated GPU per Provider
- Release: `/project/tma1/spec170-r21/ndnsf-di/releases/spec170-runtime-r22-nacabe-retry-20260818/`
- Runtime SIF SHA-256: `50478785fc9f7ef836f087e3f14954bba4982fb4196d3a6a20868c334f55eef0`
- Evidence directory:
  `/project/tma1/spec170-r21/ndnsf-di/evidence/spec170/d2b-cross-provider-spec170-r22-nacabe-retry-20260818/`

The job passed the exact-SIF staging barrier and emitted
`SPEC170_D2B_TWO_PROVIDER_GPU_RUNTIME_PASS`. The positive request completed
the real two-Provider GPU path: Provider readiness, authenticated ACK/CKey
decryption, Selection, cross-Provider `NDNSF_DATA_V1` exchange, and final
Response. The evidence directory contains both rank manifests, readiness
markers, and `d2b-positive.done`.

## Scope boundary

This job's current Slurm wrapper executes the positive D2b case only. The
Tiger evidence does **not** yet contain independent peer-mismatch, replay, and
partial-output negative rows required by T033; those cases are covered by the
local integration/security tests, but that is not a substitute for Tiger
negative evidence. Therefore T033 remains `PARTIAL`, not PASS.

The release was produced before the formal T029 freeze and must not be called a
frozen-candidate result.

## Current r23 request-ID-fix closure (2026-08-19)

The current locally built r23 SIF was rerun on the known-good pair
`itiger08,itiger09` with one GPU per Provider. All four cases completed with
exit `0:0`:

| Case | Job | Result marker | Evidence directory |
|---|---:|---|---|
| positive | 201039 | `SPEC170_D2B_POSITIVE_PASS` | `d2b-cross-provider-spec170-r23-request-id-fix-positive-knownpair-20260818-positive` |
| peer mismatch | 201040 | `SPEC170_D2B_NEGATIVE_PASS case=peer-mismatch` | `d2b-cross-provider-spec170-r23-request-id-fix-peer-mismatch-knownpair-20260818-peer-mismatch` |
| replay | 201041 | `SPEC170_D2B_NEGATIVE_PASS case=replay` | `d2b-cross-provider-spec170-r23-request-id-fix-replay-knownpair-20260818-replay` |
| partial output | 201042 | `SPEC170_D2B_NEGATIVE_PASS case=partial` | `d2b-cross-provider-spec170-r23-request-id-fix-partial-knownpair-20260818-partial` |

The release SIF is
`spec170-runtime-r23-request-id-fix-20260818/runtime.sif`, SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`.
Each run passed metadata validation, node-local staging, and staged-SIF hash
verification before entering the workload. This supersedes the pre-freeze r22
diagnostic rows above for T033; the old rows remain historical evidence.
