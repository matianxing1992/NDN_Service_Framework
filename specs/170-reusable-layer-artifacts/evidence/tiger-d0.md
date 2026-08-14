# Spec170 candidate-bound D0 CPU network workload — 2026-08-14

## Result

The D0 gate execution completed successfully on TigerCluster with Slurm job
`189483`.  The job used the candidate-bound `gate-d0-cpu.sbatch` entrypoint,
no GRES, no `--nv`, and the candidate-bound SIF.  It exercised a real NFD, the
ServiceController, four Python-binding Providers, and one Python-binding User
inside the sealed Apptainer runtime.

```text
SPEC170_D0_V3_EXECUTION_DRAIN count=4
SPEC170_D0_V3_CPU_WORKLOAD_PASS job=incontainer
SPEC170_D0_CPU_PASS jobId=189483
job state: COMPLETED
job exit: 0:0
node: itiger05
elapsed: 00:00:27
```

## Candidate and launcher binding

```text
source revision: db1601ab8614677107ba65a001cb1a029363e555
OCI: ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:29e51b62e0165b1d05c4dc5c7627da741f9d196a935bf85d76abd4f75cf28c34
SIF: /project/tma1/ndnsf-di/releases/spec170-runtime-db1601ab8614677107ba65a001cb1a029363e555/runtime.sif
SIF SHA-256: 431c2721cecd209a713ced5c9b8e8c0aa22cf8af35934f717e6824db3846a091
gate-d0-cpu.sbatch SHA-256: 8e85fb33a59d0c1de4349f92643ad110dea45c7ac62cc0b35c789e4c300a39fd
workload SHA-256: 20fe533a54950b20358d64f4c8dcde89b910a7c487f289af296fca6d6b331107
run-container.sh SHA-256: 3eb6b8dd87364e300c6aa46c55e4836c972429262ff4bfde33d11b15df1ddd11
preflight-compute.sh SHA-256: 3b7268ee34b4980529afb68b2dc7bd263feb2badf86ebc854adad6388428177b
```

The workload explicitly assigns independent ndn-cxx PIB/TPM locations to
NFD, `nfdc`, the controller, each Provider, and the User.  This reproduces
MiniNDN's per-process identity isolation inside one Apptainer instance and
prevents SQLite PIB contention.

## Observed V3 path

All four Providers reached runtime readiness and returned positive ACKs with
`DI_PLACEMENT_V3`.  User permission discovery reported `allowed=5`, then
closed the ACK window with `ackCount=4`, committed Selection, and received the
final Merge response:

```text
Backbone       CPUExecutionProvider  [1,16] 64 bytes
Head/Shard/0   CPUExecutionProvider  [1,8]  32 bytes
Head/Shard/1   CPUExecutionProvider  [1,8]  32 bytes
Merge          CPUExecutionProvider  [1,4]  16 bytes

SPEC170_V3_USER_RESPONSE ... payload=V3_OK
```

Provider artifact digests were:

```text
Backbone:     sha256:78933d8d10878d0c1590f04e269c733c40c686595ad92fd15ba78104707ff4bc
Head/Shard/0: sha256:3a8bf108bac8fddfc7edf92f5f26680e33f9c6b0e6de254eebfed43e65e6b0ef
Head/Shard/1: sha256:4cd0bed590c455b44fb5903dc7b996d5216929d40c8fd242a430e8a73dc03a28
Merge:        sha256:874a664d460b9bba8f631e1dea8d6342d391364c27c74f5deebe65813d32fd78
```

Remote evidence is retained at:

`/project/tma1/ndnsf-di/evidence/spec170/d0-v3-workload-incontainer/`

The Slurm output hash is:

```text
f19aabb0b95c7f3297e7e100b3c273e64fb930ec5036354778b11e4c9d40c8e4  slurm-189483.out
```

## Earlier failed attempts and corrections

The previous failures were deployment/harness defects, not evidence that the
NDNSF V3 path or the SIF was broken:

| Job | Failure | Correction |
|---|---|---|
| 189478 | scratch basename violated the launcher contract | use `/tmp/${SLURM_JOB_ID}` |
| 189479 | NFD could not write `/home/tma1/.ndn` | bind a writable container home |
| 189480 | four Providers shared one PIB; SQLite lock/default-identity errors | use per-role PIB/TPM locators |
| 189481–189482 | NFD and `nfdc` still shared one PIB | add a separate `nfdctl` PIB/TPM |

## Scope boundary

This proves the CPU/no-GPU D0 control, selection, and minimal ONNX execution
path for the current candidate.  Because T027/T028 and the sole-candidate
T029 freeze are not yet closed, this is not claimed as final T030 evidence.
It does not close D1 single-GPU, D2 multi-GPU/cross-Provider, hybrid
execution, Qwen multi-token generation, or the remaining T027/T029 gates.
