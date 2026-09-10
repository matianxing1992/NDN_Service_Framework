# Tiger v35 two-node evidence

**Date:** 2026-09-10
**Candidate:** APP v35 over the unchanged v22 base SIF
**Profile:** `Experiments/TigerCluster/profiles/yolo-two-node-controller-v42.json`

## Normal allocations

| Run | Slurm | Nodes | Result | Retained boundary |
| --- | --- | --- | --- | --- |
| `tiger-single-node-gpu-v35-r7` | `210340`, `COMPLETED 0:0` | `itiger02` | `PASS / NORMAL_EXPERIMENT_PASS` | 1 warmup + 1 measured request; three CUDA model roles, CPU Merge, GPU UUID, numerical oracle and clean cleanup. Verdict 230981 bytes, `sha256:86af42c1f7f79319abc32714a659e70ba8b247b094ad67517dd29329eb98d248`. |
| `tiger-two-node-gpu-v35-r5` | `210341`, `COMPLETED 0:0` | `itiger02`, `itiger03` | `PASS / NORMAL_EXPERIMENT_PASS` | 1 warmup + 3 measured requests; four roles across two nodes, CUDA model execution, CPU Merge, 9 dependency edges per request, `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.00042724609375`, and clean cleanup. Verdict 464725 bytes, `sha256:e25ee17d3e6564ebaaed7e53017f23a0565a81ddafd0a30ffb4de4a54fca4c82`. |

The normal two-node verdict is bound to candidate digest
`sha256:3bd35ac79a913eb23fd5076a669bffcf3eaebab431aac57d24dc9f2dc573b7ea`,
graph digest `sha256:d8b40347e4cb60e7a0f74b3503d04816ba897a4e8f8733e9d59a38a8c9a65ed1`,
and the same v42 profile. This closes the first normal two-node allocation only;
the reuse allocation remains pending until the registered negative case is fixed
and passes.

## Negative allocation diagnostic

Run `tiger-two-node-gpu-v35-neg1` (`210342`, `FAILED 1:0`, `itiger02`/`itiger03`)
passed allocation, both GPU probes, NFD/network readiness, Controller/Repo
readiness, provider startup, ACK and Selection. The User log contains one bound
Selection commit. DetectShard0 emitted two `NDNSF_DI_OUTPUT_WITHHELD` records for
the same producer/consumer pair, covering the graph's round-3 and round-6 output
edges; no `NDNSF_DI_NATIVE_FAILURE` record was retained from Merge and no
`negative-user.json` or `collection-input.json` was produced.

The owner command was terminated by the finite application supervisor after
`59.99460293306038` seconds. The current completion budget computes one negative
request as `permission(30 s) + request(60 s) = 90 s`, then reserves 30 s cleanup
before launching the User, leaving only 60 s. That is insufficient for the
observer's bounded 60 s dependency wait plus shutdown, so the run cannot reach
`NegativeUserObserver.finish_after_shutdown`. This is a harness timing defect,
not evidence that the negative rejection was qualified.

The second independent defect is collector cardinality: the production graph
contains two distinct DetectShard0→Merge edges, while
`read_negative_cutpoint()` requires exactly one matching producer/consumer edge
and exactly one withheld record. A rerun is blocked until the harness selects one
fully bound logical edge and gives the negative User enough completion budget;
the failed run remains immutable evidence.
