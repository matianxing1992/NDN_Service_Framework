# R11-B8-G36 Runner Scratch Name Parity

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8, T010/T011/T013/T014/T016/T017 remain open.

## Scope

This batch compared the generated Slurm template with the canonical
`run-container.sh` preflight. The template allocates
`/tmp/ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`, while the runner is also used by
direct gates that may use `/tmp/ndnsf-di-${SLURM_JOB_ID}`.

## Finding and repair

The runner previously required the exact basename
`ndnsf-di-${SLURM_JOB_ID}`. A generated multi-node job therefore passed its
supervisor scratch check and then failed at the Apptainer preflight with
`APPTAINER_SCRATCH_INVALID`, before any container was started. The mismatch is
independent of model, NFD, or Python binding behavior and would not be exposed
by a MiniNDN run that bypasses the Slurm template.

The canonical runner now accepts the exact job basename or a basename beginning
with `ndnsf-di-${SLURM_JOB_ID}-`, matching the supervisor and Spec110 contract.
The `/tmp/` root and current-job binding remain mandatory; a directory belonging
to another job is still rejected before SIF staging or Apptainer invocation.

## Validation

- `pytest -q tests/python/test_spec170_sif_build_record.py` → 9 passed. Fake-Apptainer cases cover the exact template names with and without `RUN_ID`, plus a cross-job rejection with no invocation.
- `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-container.sh` → pass.
- `git diff --check` → pass.
- No native C++ source or ABI changed; a C++ rebuild is not applicable to this shell/Python preflight.

## Review trace

The read-only `$review-agent` protocol was applied to the runner, focused test,
and Spec110/Spec182 contract diff. Review followed the scratch value from
`ndnsf-di.sbatch.in` through `run-container.sh` and checked the negative
cross-job path before SIF staging. No P1/P2/P3 finding remained after the
parity repair. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Coverage matrix

| Lane | Covered in this batch | Not covered |
| --- | --- | --- |
| Production entry/callers | Generated `ndnsf-di.sbatch.in` path and canonical runner preflight | Real Slurm submit and Apptainer runtime |
| Implementation/wire | Job ID → scratch basename rule is identical in supervisor, runner and contracts | SIF cache races and post-preflight filesystem failures |
| Test/harness/oracle | Fake-Apptainer success for both accepted names; cross-job rejection before invocation | No real SIF or multi-node process execution |
| Build/source closure | Shell syntax and Python/pytest runner tests; no C++ change | Native ABI and container ELF closure |
| Migration/evidence | Spec110/Spec182 contract, task row, checkpoint and this evidence record | Maintained caller migration, no-Python, T016/T017 and full qualification |

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to scratch-name parity. The parent Spec
remains `PARTIAL`/`UNQUALIFIED`; the next production-chain work must still
prove real SIF/Slurm execution and native requester/Provider behavior.
