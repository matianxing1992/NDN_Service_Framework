# Spec170 Tiger V3 candidate evidence — 2026-08-14

## Candidate binding

- Source revision: `920552ec82891e736e2a9eea47c22a199d447272`
- GitHub image workflow: `31799314131`
- OCI index: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:0ed360c928272316ea915940e62817a401c57ebe6883e1db757d668973064dd6`
- Linux/amd64 image manifest: `sha256:3b9e2840143673c68d53476a915612859f8b11b1eb57e5590963630d7bdb840b`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-920552ec82891e736e2a9eea47c22a199d447272/runtime.sif`
- SIF SHA-256: `sha256:eae683b4e66a80ee8889ee3c1753a738671ed8bd8e3a8a8566c3269f37264272`
- SIF size: `4,397,187,072` bytes
- Foundation image remained pinned to the previously verified Spec170 foundation digest; no system NDN-SVS or host Python extension was used.

The candidate was materialized by Job `189510`, independently verified by Job
`189511`, and statically probed by Job `189513`.

## Static exact-SIF probe

Job `189513` completed with exit `0:0` and `SPEC170_RUNTIME_STATIC_PASS`.
The SIF contained the expected `App_ServiceController`, `di-native-provider`,
`nfd`, and `nfdc` binaries. The sealed Python environment imported NDNSF and
all NDNSF-DI owner profiles, including ONNX/Qwen adapters; it reported
ONNX Runtime `1.20.0`, Torch `2.6.0+cu124`, and Transformers `4.51.0`.

## Native four-Provider network smoke

Job `189512` used real NFD, the C++ controller, four isolated native Provider
processes, the staged Python user driver, and no GPU allocation. It completed
with exit `0:0`:

```text
SPEC170_NATIVE_NETWORK_SMOKE_PASS job=189512 user_rc=0
SPEC170_NATIVE_NETWORK_SMOKE_TERMINAL exit=0 job=189512
```

Evidence under:

`/project/tma1/ndnsf-di/evidence/spec170/runtime-920552ec82891e736e2a9eea47c22a199d447272/network-bundle/network-smoke-189512/`

The user trace recorded four selected roles and the complete mapping:

```text
Backbone       -> /NDNSF-DI/Tracer/provider/backbone
Head/Shard/0   -> /NDNSF-DI/Tracer/provider/head0
Head/Shard/1   -> /NDNSF-DI/Tracer/provider/head1
Merge          -> /NDNSF-DI/Tracer/provider/merge
```

Both Head providers fetched their Backbone dependency using the producer
prefix `/NDNSF-DI/Tracer/provider/backbone/...`; there were zero
`NDNSF_DI_DEPENDENCY_WAIT_TERMINAL` markers. The user workload completed with
one successful request, payload bytes `16`, and mean response latency about
`90.67 ms` in this diagnostic workload.

This directly validates the fix for the previous failure in Job `189506`,
where per-provider Selection projections lacked the complete role-to-Provider
map and Heads requested dependencies under their own local prefixes.

## Python-binding V3 CPU/ONNX network gate

Job `189514` used the public Python V3 Provider/User scripts, real NFD and
controller, four Provider processes, CPU-only ONNX Runtime sessions, and the
same sealed SIF. It completed with exit `0:0`:

```text
SPEC170_PYTHON_V3_NETWORK_PASS job=189514 user_rc=0
SPEC170_PYTHON_V3_NETWORK_TERMINAL exit=0 job=189514
SPEC170_V3_EXECUTION_DRAIN count=4 graceMs=5000
```

All four roles emitted `CPUExecutionProvider` readiness and execution markers,
with `cpuFallbackDisabled=false` because CPU is the explicitly selected
backend. The User observed permission discovery, four ACKs, ACK closure,
V3 Selection commit, and a Merge response with payload:

```text
schema=spec170-v3-response-v1
payload=V3_OK
provider=/NDNSF-DI/Tracer/provider/merge
role=/Merge
```

## Scope and remaining gates

These jobs prove the new candidate's exact SIF closure, CPU/no-GPU startup,
real ONNX artifact loading/execution, multi-Provider ACK/Selection, native
cross-Provider dependency naming, and Python V3 response delivery. They do
not yet prove one/two-GPU allocation, canonical artifact publication and
reuse, `NDNSF_DATA_V1` cross-Provider transfer, protected-artifact security,
or heterogeneous `[1,2,1]`/`[2,1,2]` hybrid execution. Those remain the next
Spec170 gates and must use the same frozen candidate identity.

