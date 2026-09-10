# R11-B8-G39 Compute Preflight Scratch Parity

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8, T010/T011/T013/T014/T016/T017 remain open.

## Finding and repair

The active Slurm templates render a scratch basename of
`ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`. The canonical `run-container.sh` already
accepts that job-bound suffix, but `preflight-compute.sh` still required the
older exact `/tmp/${SLURM_JOB_ID}` path. A real job therefore failed before
Apptainer was invoked, while MiniNDN did not exercise the compute preflight.

The preflight now requires a direct child of `/tmp` whose basename is exactly
`ndnsf-di-${SLURM_JOB_ID}` or starts with that value plus `-`. Another job,
nested paths, and an empty basename fail before Apptainer. The Spec110
allocation contract records the same requirement for the rendered template.

## Validation

- `python3 -m pytest -q tests/container/unit/test_slurm_node_scripts.py` → 5 passed.
- `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-compute.sh` → pass.
- `git diff --check` → pass.
- No C++ or ABI change; native rebuild is not applicable to this shell preflight.

## Review trace

The read-only `$review-agent` protocol was applied to the preflight, its
template callers, the new negative/positive tests, and the Spec110 contract.
The review checked basename matching, nested-path rejection, failure ordering
before Apptainer, and compatibility with both active template scratch forms.
No P1/P2/P3 finding remained. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Coverage matrix

| Lane | Covered in this batch | Not covered |
| --- | --- | --- |
| Production entry/callers | `preflight-compute.sh` and both active Slurm templates | Real Slurm controller and compute-node filesystem |
| Implementation/wire | Job ID plus optional run-id suffix parity with runner/topology | Allocation mutation after preflight |
| Test/harness/oracle | Five focused tests with fake Apptainer, other-job and nested negatives | Real Apptainer/SIF execution |
| Build/source closure | Shell syntax and diff checks; no C++ change | Native ABI, SIF/ELF closure and GPU driver injection |
| Migration/evidence | Spec110 contract, Spec182 task/audit/evidence synchronization | Maintained callers, no-Python, T016/T017 and full qualification |

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to compute-preflight scratch parity. The
parent Spec remains `PARTIAL`/`UNQUALIFIED`; exact-SIF process wrapping and
real multi-node qualification remain required.
