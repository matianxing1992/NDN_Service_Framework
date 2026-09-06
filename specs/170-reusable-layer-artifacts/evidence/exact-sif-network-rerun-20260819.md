# Exact-SIF network rerun (2026-08-19)

This record captures the first local exact-SIF execution of the retained
network gates. It is qualification evidence for the sealed r23 source, not a
T029 freeze or a claim about the current dirty working tree.

## Identity and invocation

```text
SIF:              .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SIF SHA-256:      5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
sourceRevision:   989a9daace669a4f93496dade3176c527edb2469
sourceSeal:       sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e
Apptainer:        /opt/apptainer/1.5.3/bin/apptainer (1.5.3)
bundle:           .codex-tmp/spec170-container-build-20260818-r14/r18-network-bundle
```

The test process resolved the explicit `/opt/apptainer/1.5.3/bin/apptainer`
path through the gate's `SPEC170_APPTAINER` contract. The ambient
`/usr/local/bin/apptainer` reports 1.3.4 and was not used.
The retained four-role bundle contains `nfd.conf.in`, all four ONNX artifacts,
the Controller policy, trust schema, native plan, service manifest, and User
driver required by the D0/D1 workload scripts.

## Commands and results

```bash
export SPEC170_EXACT_SIF="$PWD/.codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif"
export SPEC170_EXACT_SIF_BUNDLE="$PWD/.codex-tmp/spec170-container-build-20260818-r14/r18-network-bundle"
export SPEC170_EXPECTED_RUNNER_KIND=onnxruntime-cpu
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_real_minindn_gate.py -k exact_sif -vv
```

Result: **4 passed, 8 deselected, 23.43 s**.

| Gate | Result | Required observation |
|---|---|---|
| Apptainer identity/version | PASS | gate selects `/opt/apptainer/1.5.3/bin/apptainer` and verifies 1.5.3; it does not trust the ambient PATH |
| Provider CLI contract | PASS | exact SIF exposes `--bootstrap-token` and `--execution-policy` |
| D0 four-Provider dependency chain | PASS | Request → ACK → Selection → Response; four dependency edges published/fetched; CPU ONNX path executed |
| D1 single-Provider dependency chain | PASS | same complete four-edge lifecycle with the single-provider CPU runner |

## Bundle-selection failure caught before execution

An earlier diagnostic invocation paired the D0/D1 tests with the D2b bundle
`r23-d2-bundles-v26-request-id-fix/d2b`. The gate stopped immediately with:

```text
SPEC170_D0_CURRENT_BUNDLE_FAIL missing=.../d2b/nfd.conf.in
```

That bundle is a two-Provider D2b bundle and contains `nfd.conf`, not the
four-role D0/D1 input set. The failure is therefore a bundle-selection/input
contract error, not a SIF or protocol failure. It is retained as a useful
pre-execution negative check: D0/D1 gates must reject a D2b bundle rather than
silently running an incomplete topology.

## Scope

These runs prove that the sealed r23 image can execute the real D0/D1 request
lifecycle with the correctly typed external bundle. They do not qualify the
current uncommitted source, CUDA/Tiger behavior, T029, the full T018/3C
mutation matrix, or T036 performance optimality. The bundle and SIF must still
be bound together in a future frozen candidate manifest.
