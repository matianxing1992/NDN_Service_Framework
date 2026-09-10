# R11-B8-G35 Scheduler Allocation Order Binding

**Date**: 2026-09-10  
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8, T010/T011/T013/T014/T016/T017 remain open.

## Scope

This batch audited the real multi-node launch relationship between the frozen
process map and the two shell entry points that invoke `srun --relative`:
`run-allocation-topology.sh` and `configure-allocation-routes.sh`. MiniNDN's
deterministic node creation order can hide a different Slurm allocation order.

## Finding and repair

Before this batch, both scripts trusted `nodeRank` without checking the
allocation hostname sequence. A map generated as `node-a, node-b` could be
executed against a Slurm allocation returned as `node-b, node-a`, placing the
NFD, identity, or route command on the wrong machine while address and port
syntax checks still passed.

`allocation_topology.validate_allocation_node_order` now compares the map's
rank-ordered node names with `scontrol show hostnames
"$SLURM_JOB_NODELIST"`. Both entry points fail before creating node state or
issuing NFD/route commands when the scheduler query is missing, fails, empty,
or differs. The scheduler query exit status is checked explicitly so a failed
`scontrol` command cannot be hidden by process substitution.

## Validation

- `python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py` → 25/25 tests, `OK`.
- `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` → expected reversed-order pre-start failure and final `NETWORK_SCRIPT_PASS`.
- `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh Experiments/TigerCluster/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh` → pass.
- `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py tests/container/itiger-qwen-live/unit/test_allocation_topology.py` → pass.
- `git diff --check` → pass.
- The earlier `python3 -m unittest tests/container/.../unit/test_allocation_topology.py` invocation was an invalid import-mode command (`ModuleNotFoundError`); the supported direct-file test above is the product result.

## Review trace

The read-only `$review-agent` protocol was applied to the complete seven-file
source/test/contract diff at the current pre-checkpoint baseline. Review scope
covered both production callers, the new Python validator, test fixtures and
the Spec110/Spec182 contract updates. No P1/P2/P3 finding was introduced.
The official skill source was `/home/tianxing/.codex/skills/review-agent/SKILL.md`
with SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Coverage matrix

| Lane | Covered in this batch | Not covered |
| --- | --- | --- |
| Production entry/callers | Supervisor and direct route retry both use the shared allocation-order validator before `--relative` execution | A real Slurm controller was not available on this host |
| Implementation/wire | `nodeRank` → `scontrol` hostname sequence → `srun --relative` binding; explicit query status handling | Scheduler-side allocation changes after preflight remain outside the frozen map contract |
| Test/harness/oracle | Same-order pass, reversed-order fail, zero-NFD assertion, source assertions, shell syntax | No multi-host network or NFD route exchange |
| Build/source closure | Python compile and diff checks; no C++ source changed | No native rebuild required for this shell/Python boundary |
| Migration/evidence | Spec110 and Spec182 contracts, tasks row, checkpoint and this evidence record | T013 maintained callers, no-Python, T016/T017 and deployment qualification remain open |

## Batch retrospective

- **Static review**: caught the MiniNDN-vs-Slurm ordering assumption; explicit
  `scontrol` status handling was added after reviewing process-substitution exit
  semantics.
- **Compile/link**: not applicable; no C++ or ABI change.
- **Runtime/test**: focused topology unit and network-script integration pass;
  the negative case proves rejection before NFD state is created.
- **Unobserved**: real Slurm hostname expansion, SIF execution, cross-node NDN
  faces/routes, GPU visibility, and no-Python runtime remain unobserved.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to this scheduler-order pre-start boundary.
The parent Spec remains `PARTIAL`/`UNQUALIFIED`; the next batch must address
the independently reproduced scratch-name contract mismatch in the canonical
`run-container.sh` path before claiming multi-machine readiness.
