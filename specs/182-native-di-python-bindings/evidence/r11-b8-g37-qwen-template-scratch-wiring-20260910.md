# R11-B8-G37 Qwen Template Scratch Wiring

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8, T010/T011/T013/T014/T016/T017 remain open.

## Scope

This batch traced the shared packaging Qwen Slurm template through its
`@@RUN_CONTAINER@@` invocation into the canonical `run-container.sh` preflight.
Although the template is retained from the earlier Qwen profile, it remains a
maintained entry point used by the packaged runtime and delivery quickstart.

## Finding and repair

The template used `/tmp/$USER/ndnsf-di/$SLURM_JOB_ID` while the canonical runner
requires a `/tmp/ndnsf-di-<SLURM_JOB_ID>` basename (with an optional run-id
suffix). A real job would therefore fail with `APPTAINER_SCRATCH_INVALID` after
all earlier work had succeeded. The old path is not exercised by MiniNDN.

The template now declares `RUN_ID` and generates
`/tmp/ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`, which is accepted by the runner and
matches the supervisor and allocation-topology contract. Existing cleanup and
evidence staging use the same variable-bound path.

## Validation

- `pytest -q tests/container/itiger-qwen-live/unit/test_github_sealed_workflow.py` → 31 passed; the test asserts the canonical scratch expression and rejects the old `$USER/ndnsf-di/$SLURM_JOB_ID` form.
- `git diff --check` → pass.
- No C++ source, native ABI, or SIF content changed; a native rebuild is not applicable to this template wiring boundary.

## Review trace

The read-only `$review-agent` protocol was applied to the template, its
canonical runner caller, the regression test and the existing Spec110/Spec182
contract. The review followed the literal scratch value across template
rendering, `run-container.sh` validation, cleanup and evidence staging. No
P1/P2/P3 finding remained after the repair. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Coverage matrix

| Lane | Covered in this batch | Not covered |
| --- | --- | --- |
| Production entry/callers | `ndnsf-qwen.sbatch.in` → `run-container.sh` scratch argument | Real Slurm rendering/submission |
| Implementation/wire | `RUN_ID` and `SLURM_JOB_ID` form one runner-accepted path; cleanup uses the same shell variable | Runtime filesystem failure after preflight |
| Test/harness/oracle | Sealed-workflow source regression, including old-path rejection | No real SIF or multi-node execution |
| Build/source closure | Template/static Python test and diff check; no native change | SIF/ELF/ABI closure |
| Migration/evidence | Spec182 task/checkpoint/audit/evidence and existing Spec110 contract | Maintained caller migration, no-Python, T016/T017 and qualification |

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to the shared Qwen template's scratch
wiring. The parent Spec remains `PARTIAL`/`UNQUALIFIED`; real Slurm/SIF and
native requester/Provider qualification remain required.
