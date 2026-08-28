# Job 182780: bounded large-model failure

Job `182780` was the single admitted v93 replacement for the pinned Qwen3.6-
27B three-node request. It used source bundle
`sha256:96f9bec36b3f08fae893c26da6de3ba7cc99f5b11fd2705b7e7e207153535ad5`,
source identity
`sha256:aa48886cced5832bdb5b45d0a201422b672bb8197c3348ba022f14eea3344217`,
the exact SIF
`sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`,
and large stage manifest
`sha256:16de41986564c436a33cd6a292f17f4bd939d0a893c441441022e3b389737f6f`.

The v93 repository-identity fallback repaired the earlier
`SPEC162_SELECTION_MODEL_TYPE_UNSUPPORTED` failure from Job 182778. Job 182780
passed source/SIF/stage/artifact checks, secured ACK closure, graph/placement,
Selection commit, and began the real 18.53-GB Stage-0 Repository fetch. The
fetch reached 18,530,194,174 verified bytes with zero retransmitted bytes.

It did **not** produce a model response. While Stage 0 was preparing the model,
the TigerCluster kernel recorded a memory-cgroup OOM kill for Provider process
`pid=1161747`:

```text
oom-kill: constraint=CONSTRAINT_MEMCG ... job_182780 ... task=python,pid=1161747
Memory cgroup out of memory: Killed process 1161747
total-vm:45114472kB anon-rss:16987456kB
```

Slurm records the rank step as `OUT_OF_MEMORY` (`182780.0`, exit `0:125`), with
the batch subsequently cancelled after no progress. The retained remote
directory is:

`/project/tma1/ndnsf-di/evidence/spec168/tiger-large-single/`
`spec168-campaign-v3-8c75d2b72279bf9d6887-FAILED-job-182780-oom`

This is an environmental/model-preparation failure, not a successful large-
model qualification. Per the campaign contract no further large-model retry is
admitted. T015 therefore remains open; no claim of complete Qwen3.6 inference
or three-stage CUDA response is made.
