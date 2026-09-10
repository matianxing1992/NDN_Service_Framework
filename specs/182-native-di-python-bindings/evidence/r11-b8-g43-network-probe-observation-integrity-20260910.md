# R11-B8-G43 Network Probe Observation Integrity

**Date**: 2026-09-10  
**Status**: `CLOSED_FOR_VALIDATION` for the deployment-harness boundary; parent R11-B8,
T010/T011/T013/T014/T016/T017 remain open.

## Scope

This review followed every live `srun --relative=<nodeRank>` network probe and the
`evaluate_transport_probe` observation contract. MiniNDN and the offline fixture evaluator
previously hid three multi-node assumptions: the live script copied map addresses into its own
observation, an observation file could be supplied in a real Slurm environment to bypass live
checks, and a v1 map could assign one address to two nodes when their ports differed. The
cross-node input digest also covered paths and bytes but not permission modes.

## Finding and repair

- The live probe now runs a bounded target-node UDP route probe for each rank, records the source
  IPv4 selected by that node's routing table, and compares the observed list with map addresses in
  rank order. It no longer self-reports the expected values.
- `NDNSF_SPEC110_PROBE_OBSERVATION` is accepted only with
  `NDNSF_SPEC110_TEST_MODE=1`; a real Slurm job receives
  `SPEC110_PROBE_OBSERVATION_REQUIRES_TEST_MODE` before creating output or running a bypass path.
- Process-map validation rejects a reordered `nodes` array, duplicate node addresses even when
  TCP/UDP ports are distinct, and transport observation values that are not list/order exact.
- Each live diagnostic `srun` has a bounded timeout; timeout is classified as a closed route. The
  workdir/identity digest now includes root and regular-file permission bits and rejects a symlink
  at the digest root.

These checks preserve the existing distinction between a transport diagnostic and an NDN protocol
or business-request qualification.

## Verification

| Check | Result |
| --- | --- |
| `python3 -m pytest -q tests/container/itiger-qwen-live/unit/test_allocation_topology.py tests/container/itiger-qwen-live/unit/test_multinode_probe.py tests/container/unit/test_slurm_node_scripts.py tests/python/test_spec170_sif_build_record.py tests/container/itiger-qwen-live/unit/test_github_sealed_workflow.py` | **84 passed** |
| `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` | **`NETWORK_SCRIPT_PASS`**; real-job observation bypass, pre-start digest mismatch, overlap and existing network failures remain fail-closed |
| `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh` | pass |
| `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py` | pass |
| `git diff --check` | pass |

No C++ source, ABI, SIF, or native runtime behavior changed; a native rebuild is not applicable.
The fake `srun` and offline observation fixture do not qualify real Slurm, SIF, GPU, NDN face/route,
no-Python, or T016/T017 behavior.

## Review trace

The read-only `$review-agent` protocol was applied to the complete script/library/contract/test
diff and callers. It checked observation provenance, rank/address binding, timeout/error handling,
test-mode isolation, digest compatibility, and evidence synchronization. No P0/P1/P2/P3 finding
remained. The official skill source was
`/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to observation provenance, node-address uniqueness, bounded
network diagnostics, and input permission-mode consistency. The parent Spec remains
`PARTIAL`/`UNQUALIFIED`; real multi-node runtime and native requester/Provider qualification remain
required.
