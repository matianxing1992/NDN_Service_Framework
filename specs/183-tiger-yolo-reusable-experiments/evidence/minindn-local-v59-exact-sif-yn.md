# Exact-SIF local MiniNDN v59 Y-N aggregate

**Date:** 2026-09-09

**Run:** `minindn-local-20260909-v59-yn-final`

**Case:** `Y-N`

**Verdict:** `T010_DONE`, `returncode=0`, aggregate `PASS`,
`qualification=NOT_EVALUATED`

This run is the fresh post-fix aggregate requested by T010. It uses the same
immutable base/application composition as v58 and exercises the maintained
Spec183 MiniNDN driver. It is local CPU/MiniNDN evidence; it does not qualify a
GPU, Slurm allocation, or TigerCluster run.

## Frozen composition

- base SIF: `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`
- external APP manifest: `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`
- dispatch plane: `sha256:f4bdeb5df7292ee4c123d6082a3be1db26dd4ae32eb6d95d0ed17223194fd78d`
- runtime plane: `sha256:ad10ff7cc05bf091315c14bc4af495c50f348d4f7080dd089f7061adf9983ddc`
- profile digest: `sha256:f87fefef7e1fe288696495e04c4010216ea3e25f2a5cd7dbc96f161c27a39e0c`
- prepared candidate: `sha256:eb60bb4b25f452dae30341f6599d3af8fae5530ba5e892ee8dfd5a7dbc0d0349`
- preparation receipt: `sha256:47d0764396cf6522062de08c4c606e660e854df5cb91ffb182e101c94b00955d`
- runtime version: Apptainer `1.5.3` for issuer and rank 0

The application layer was mounted read-only at `/app`; the four native roles
were `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`. Preparation
was performed once for this run with the maintained `stage_provision_inputs`
and `provision_run` boundary before the MiniNDN driver started.

## Command and supervisor result

The maintained command was:

```text
python3 -u Experiments/TigerCluster/tools/spec183_minindn.py \
  --run-id minindn-local-20260909-v59-yn-final \
  --output Experiments/TigerCluster/results \
  --profile Experiments/TigerCluster/profiles/yolo-two-node-controller-v33.json \
  --preparation-sha256 sha256:47d0764396cf6522062de08c4c606e660e854df5cb91ffb182e101c94b00955d \
  --case Y-N
```

The systemd owner retained `runtimeSeconds=900`, `clientWaitSeconds=945`,
`cleanupSeconds=30`, `returncode=0`, no errors, and `processCleanup=CLEAN`.
The retained run evidence is under the ignored result directory
`Experiments/TigerCluster/results/minindn-local-20260909-v59-yn-final/`.

## Aggregate result

All eight registered subcases passed:

| Subcase | Boundary | Outcome | Reason |
| --- | --- | --- | --- |
| Y-N-O | `TERMINAL_RESPONSE` | `CONTROL` | `TERMINAL_RESPONSE_VERIFIED` |
| Y-N-C | `PLACEMENT_DECISION` | `FAIL_CLOSED` | `NO_FEASIBLE_CANDIDATE` |
| Y-N-P | `ACK_CLOSED` | `FAIL_CLOSED` | `ACK_PROVENANCE_REJECTED` |
| Y-N-R | `PLAN_SEALED` | `FAIL_CLOSED` | `ROLE_KIND_REJECTED` |
| Y-N-I | `PROVIDER_EXECUTION_STARTED` | `FAIL_CLOSED` | `NON_INGRESS_INPUT_REJECTED` |
| Y-N-E | `PROVIDER_GRANT_VERIFICATION` | `FAIL_CLOSED` | `DI_PROTECTED_GRANT_REJECTED` |
| Y-N-L | `EVIDENCE_ACCEPTANCE` | `FAIL_CLOSED` | `REDACTION_REJECTED` |
| Y-N-D | `DEPENDENCY_DATA_MISSING` | `FAIL_CLOSED` | `DEPENDENCY_DATA_MISSING` |

Y-N-E independently recorded the three registered mutation variants
`EXPIRED`, `WRONG_RECIPIENT`, and `FORGED_AUTHORITY`. Y-N-D retained a native
DetectShard0→Merge withheld-Data record after Selection, so the negative result
is tied to the declared dependency cutpoint rather than an unrelated timeout.
The aggregate file is
`host-minindn/output/y-n-matrix-result.json` with digest
`sha256:f7d8ad40ea64f6fcc636b00fd91a7de78318f44517ff3bcd7785d7f7cc420706`.

## Scope and next gate

This closes the fresh Y-N aggregate and completes the local MiniNDN negative
matrix at the declared scope. It does not change the host receipt's
`YOLO_HOST_GATE_COMPONENT_ONLY` qualification, and it does not close T007's
formal ordering gate. The paired v60 [empty-HOME/scratch isolation probe](minindn-local-v60-empty-home-scratch.md)
now closes that local boundary. The next gate is one content-addressed staging
of this exact SIF and APP to Tiger, followed by a bounded single-node GPU
preflight and request.
