# R11-B8-G44 Frozen Topology and Probe Count Integrity

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8,
T010/T011/T013/T014/T016/T017 remain open.

## Scope

The review compared the supervisor's map reads with its generated launchers and the
`evaluate_transport_probe` observation contract. A mutable submit-host process map could be
replaced after the first validation, causing later rank, port, or route lookups to describe a
different topology. The evaluator also accepted a malformed but superficially successful row:
unknown status values, arbitrary ports, boolean counts, or a `reachableRoutes` value unrelated to
the validated route list. Exact duplicate route tuples would repeat face/route mutations.

## Repair

- `run-allocation-topology.sh` copies the validated map to job scratch with mode `0400`; every
  later process, rank, port, placement, and route lookup consumes that frozen snapshot. A second
  allocation-order check covers a map replacement between the first scheduler check and render.
- `validate_process_map` rejects malformed route node ranks and exact duplicate route tuples before route configuration; malformed ranks cannot escape as a raw Python `TypeError`.
- `evaluate_transport_probe` validates `PASS`/`FAIL`, list and declared-port membership for
  `closedPorts`, non-boolean integer counts, count bounds, and the exact relation
  `reachableRoutes = routeCount - len(closedPorts)`. `PASS` requires an empty closed-port list.

## Verification

| Check | Result |
| --- | --- |
| `python3 -m pytest -q tests/container/itiger-qwen-live/unit/test_allocation_topology.py tests/container/itiger-qwen-live/unit/test_multinode_probe.py tests/container/unit/test_slurm_node_scripts.py tests/python/test_spec170_sif_build_record.py tests/container/itiger-qwen-live/unit/test_github_sealed_workflow.py` | **88 passed** |
| `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` | **`NETWORK_SCRIPT_PASS`**; frozen map and mode-0400 checks pass |
| `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh` | pass |
| `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py` | pass |
| `git diff --check` | pass |

No C++ source, ABI, SIF, or native runtime behavior changed; a native rebuild is not applicable.
The fake `srun` and offline observation fixture do not qualify real Slurm, SIF, GPU, NDN face/route,
no-Python, or T016/T017 behavior.

## Review trace and closure

The read-only `$review-agent` protocol was applied to the complete library/supervisor/contract/test
diff and all current callers. It checked snapshot ownership, map mutation windows, duplicate-route
effects, observation typing/counts, failure codes, cleanup, and evidence synchronization. No
P0/P1/P2/P3 finding remained. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

`CLOSED_FOR_VALIDATION` applies only to frozen-map and transport-observation integrity. The parent
Spec remains `PARTIAL`/`UNQUALIFIED`; exact-SIF process wrapping, real multi-node runtime and
native requester/Provider qualification remain required.
