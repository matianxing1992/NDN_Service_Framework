# R11-B8-G38 Network Probe Allocation Order Binding

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8, T010/T011/T013/T014/T016/T017 remain open.

## Scope

The G35 audit protected the topology supervisor and direct route retry, but a
repository-wide search found a third live entry point,
`probe-multinode-network.sh`, that also issues `srun --relative=<fromNodeRank>`
for TCP and UDP diagnostics.

## Finding and repair

Without the same scheduler-order check, a diagnostic run could probe a different
node than the supervisor and route scripts, producing a misleading reachability
result or validating the wrong endpoint. The live probe now compares the
rank-ordered process-map names with `scontrol show hostnames
"$SLURM_JOB_NODELIST"`, checks the query exit status, and fails before any
diagnostic `srun` or output observation is written. The offline observation-file
branch remains a fixture evaluator and intentionally bypasses Slurm.

## Validation

- `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` → reversed-order live-probe rejection and final `NETWORK_SCRIPT_PASS`.
- `python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py` → 25/25 tests, `OK`.
- `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh` → pass.
- `git diff --check` → pass.
- No C++ or ABI change; native rebuild is not applicable to this diagnostic preflight.

## Review trace

The read-only `$review-agent` protocol was applied to all `srun --relative`
callers found by the repository search: supervisor, route configuration and
network probe. The review checked that the observation-file fixture path is
separate from live Slurm execution and that the new guard runs before network
subprocesses. No P1/P2/P3 finding remained. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Coverage matrix

| Lane | Covered in this batch | Not covered |
| --- | --- | --- |
| Production entry/callers | Live network probe and its TCP/UDP diagnostic `srun --relative` calls | Real Slurm controller and cross-node sockets |
| Implementation/wire | Process-map rank → scheduler hostname order before each diagnostic lane | Scheduler allocation mutation after preflight |
| Test/harness/oracle | Reversed-order fail before output, existing fixture evaluator, network integration | Real NFD face/route exchange |
| Build/source closure | Shell syntax, topology unit and diff checks; no C++ change | Native ABI and SIF/ELF closure |
| Migration/evidence | Spec110/Spec182 contract, task/checkpoint/audit/evidence synchronization | Maintained callers, no-Python, T016/T017 and full qualification |

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to network-probe scheduler-order binding.
The parent Spec remains `PARTIAL`/`UNQUALIFIED`; real multi-node runtime and
native requester/Provider qualification remain required.
