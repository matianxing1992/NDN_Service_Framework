# Spec170 Python contract rerun (2026-08-19)

This is the latest local contract-lane result after adding the candidate-input
inventory checks. It is current-worktree diagnostic evidence; it is not proof
that the r23 SIF contains the uncommitted changes.

## Command

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_*.py -rs
```

Environment: host Python 3.8.10, pytest 8.3.5. No exact-SIF or real-Qwen
environment variables were used for this contract lane.

## Result

```text
105 passed, 10 skipped, 1 warning in 6.10 s
```

The ten skips are explicit external-environment gates: four exact-SIF checks,
four real NativeTracer/MiniNDN checks, and one cached Qwen multi-request check
(plus the remaining real MiniNDN smoke gate). The two-test candidate-input
inventory lane is included in the 105 passed total. This result supersedes the earlier 103-pass snapshot
for the count of the current test glob; older evidence files retain their
original run counts.

The result establishes no unexpected Python contract failure. It does not
close T018/T028/T029/T036, prove production CUDA equivalence, or replace the
exact-SIF lifecycle evidence in `exact-sif-network-rerun-20260819.md`.

## Full glob with exact-SIF inputs

The same glob was rerun with the sealed r23 image, the retained four-role
bundle, and the explicit Apptainer variables:

```bash
export SPEC170_EXACT_SIF="$PWD/.codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif"
export SPEC170_EXACT_SIF_BUNDLE="$PWD/.codex-tmp/spec170-container-build-20260818-r14/r18-network-bundle"
export SPEC170_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer
export SPEC170_APPTAINER_VERSION=1.5.3
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_*.py -rs
```

Result: **109 passed, 6 skipped, 1 warning in 28.81 s**. All four exact-SIF
tests now execute inside the glob; the six remaining skips are the cached
Qwen multi-request and real NativeTracer/MiniNDN gates. This strengthens the
exact-SIF qualification but still does not close the full T028/T036 or T029
requirements.

After adding the exact-SIF Provider process-census assertion and the D0 direct
`exec env` launch contract, the same exact-image glob reported:

```text
110 passed, 6 skipped, 1 warning in 26.66 s
```

The exact-SIF D0 and D1 dependency workloads also passed together, with zero
remaining `/opt/ndnsf-di/current/bin/di-native-provider` processes after the
test command. This strengthens harness cleanup evidence only; it does not
change r23's sealed-source boundary.

After adding the dependency-trace regression for Python
`planned_name=true|false` markers and actual `data_name` URIs, the same exact
SIF glob reported:

```text
111 passed, 6 skipped, 1 warning in 29.47 s
```

This extra pass validates the evidence parser contract; it does not add the
current dirty-tree C++ logger change to r23.

## Fresh current-tree rerun

After the current-source post-selection and DATA_V1 regressions were rebuilt,
the host contract glob was rerun without exact-SIF or real-environment
variables:

```text
107 passed, 10 skipped, 1 warning in 7.47 s
```

The compact log SHA-256 is
`9c55c76543fdcc06ebe2058736b03bcad058b3cccd511b1618b44d0903ab8fc3`.
The additional two collected tests are current contract tests; the result is
still diagnostic evidence for the dirty tree and does not qualify the r23 SIF
or close T028, T029, T036, or T037.
