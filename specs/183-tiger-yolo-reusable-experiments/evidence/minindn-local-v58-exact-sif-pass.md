# Exact-SIF local MiniNDN v58

**Date:** 2026-09-09
**Run:** `minindn-local-20260909-v58-final`
**Case:** `local-cpu`
**Verdict:** `status=PASS`, `qualification=NORMAL_EXPERIMENT_PASS`

This is the first formal local `submit.py local` collector PASS for the
layered Spec183 composition. It is a local CPU/MiniNDN result only; it does
not qualify a GPU, Slurm allocation, or TigerCluster run.

## Frozen composition

- base SIF: `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`
- external APP manifest: `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`
- dispatch plane: `sha256:f4bdeb5df7292ee4c123d6082a3be1db26dd4ae32eb6d95d0ed17223194fd78d`
- runtime plane: `sha256:ad10ff7cc05bf091315c14bc4af495c50f348d4f7080dd089f7061adf9983ddc`
- prepared candidate: `sha256:28baa5e0d2eb88b04fd49ab208b01dd019d13c85c5d43ab6d472181cde23ee9c`
- runtime version: Apptainer `1.5.3` for issuer and rank 0

The run used the read-only `/app` application layer with four native
providers: `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`.

## Observed result

The two requests (one warmup and one measured) both completed the real
MiniNDN lifecycle: ACK/Selection, protected grants, four-provider execution,
cross-role tensor transfer, terminal response, and process-group cleanup.
The retained numerical records both report `shape=[1,50,6]`,
`matched=true`, and `maxAbsError=0.0005340576171875` with `atol=0.001`.
The collector paired all 9 declared dependency edges for each request. The
native role records contain 6, 6, 12, and 12 dependency objects for
`BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`, respectively.
The Merge observation is the expected `native-yolo-postprocess` owner with no
ORT load or warmup; the three model roles report ORT CPU load, warmup, and
real compute.

Retained machine evidence is under the ignored result directory:
`Experiments/TigerCluster/results/minindn-local-20260909-v58-final/`.
`verdict.json` is the collector authority; `node0/node-receipt.json`, the
per-role logs, both `yolo-public-assignments.json` files, lifecycle records,
and numerical records are the supporting evidence.

## Fixes required to reach PASS

The previous v55 run launched the same four-provider graph and returned the
correct YOLO tensor, but collection stopped because `NDN_LOG` suppressed the
backend-owned `ndnsf.di.RuntimeEvidence` logger. v56 retained those markers,
then exposed that native logs use `publish-exact-ndn`/`fetch-exact-ndn`, keep a
leading slash in the request session, omit the `/attempt/<n>` suffix, and
encode integer Name components as ndn-cxx NNIs (`%00`, `%01`) while the Python
public projection uses decimal text. The collector now normalizes these
wire-format differences while retaining exact role, edge, status, byte-count,
and request-prefix checks. The frozen v58 bundle includes these fixes.

## Remaining gates

The fresh aggregate Y-N follow-up is recorded in
[v59 exact-SIF Y-N aggregate](minindn-local-v59-exact-sif-yn.md), including a
zero-exit `T010_DONE` and all registered control/negative subcases. T011
empty-HOME and scratch isolation remains a separate check. Tiger exact-SIF
staging, one-node GPU, and two-node GPU qualification have not run; the old
remote SIF staging attempt was canceled and must be replaced by one verified
content-addressed copy before submission.
