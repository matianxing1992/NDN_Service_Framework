# Exact-SIF local MiniNDN v62 localSif gate

**Date:** 2026-09-09  
**Run:** `minindn-local-20260909-v62-local-gate`  
**Case:** `local-cpu`  
**Verdict:** `status=PASS`, `qualification=NORMAL_EXPERIMENT_PASS`

This run revalidated the unchanged v22 base plus v32 application with the
remote compute-node Apptainer contract frozen as `1.5.3-1.el9`. It is the
`localSif` prerequisite for the next single-node GPU submission; it is not a
GPU or Tiger qualification.

## Frozen identities

- profile document: `sha256:f35ca35a0ca69e25f340d94a8626fe3f1060e1fc8dfccebaaf21e174979d8890`
- inputs plane: `sha256:4590fec0844c0d83eebd7fc49cfd90e6f240edab5de627d1a22894b6bd32b828`
- runtime plane: `sha256:ad10ff7cc05bf091315c14bc4af495c50f348d4f7080dd089f7061adf9983ddc`
- dispatch plane: `sha256:9f9191e654bf2f4992b3df2971172493e86507b97bafd61b54c8a73ca0eb54a7`
- base SIF: `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`
- external APP manifest: `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`
- prepared candidate: `sha256:448e85898890620919c3748319f4ca6a6ad6f1d10e7bc382698c0d4c53c74c33`
- collection input: `sha256:11089ca3aad8a4cda760abdcb27a810a74dfa0e5b1144f82bdde70f8aee5a2db`

The exact base SIF and read-only `/app` bundle were reused. Both issuer and
rank runtime receipts observed Apptainer `1.5.3`; the profile's remote value
is `1.5.3-1.el9`, matching the allocated Tiger compute package suffix.

## Observed result

Two real cross-process requests (one warmup and one measured) completed
ACK/Selection, protected grants, four-provider execution, dependency transfer,
terminal response, and cleanup. Both numerical records report
`shape=[1,50,6]`, `matched=true`, and `maxAbsError=0.0005340576171875` with
`atol=0.001`. The three model roles used `CPUExecutionProvider`; Merge used
the native `native-yolo-postprocess` owner. The retained node receipt reaped
all 14 children, released every lease, and used no forced cleanup.

The full retained evidence is under the ignored result directory
`Experiments/TigerCluster/results/minindn-local-20260909-v62-local-gate/`;
`verdict.json` is the collector authority.

## Scope

This closes the v34 profile's local CPU `localSif` gate and supplies a fresh
candidate identity for the single-node GPU gate. It does not establish CUDA,
Slurm allocation, project staging, or Tiger runtime success.
