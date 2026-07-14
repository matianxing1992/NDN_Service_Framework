# iTiger07 account and Apptainer preflight: job 150145

## Verdict

`PASS` — the explicitly authorized one-minute CPU-only preflight completed on
`itiger07`. The node resolved `tma1` / UID `64102` and started its installed
Apptainer successfully. This isolates job `149669`'s `unknown userid 64102`
failure to `itiger02` or a node-local/transient account-resolution condition;
it is not evidence of a globally invalid iTiger account.

## Frozen identity

- Candidate: `spec110-c1-48b6832b59c1-dddb41e5c89c-d81aeb36fe1f-cf2c9ac4172e-873f800e3fab-e6e3e20516ea`
- Cell: `spec110-cell-ce67a15ee3bd317ad54f`
- Run: `spec110-run-40c4f52f8203dc21eff5`
- Submission: `spec110-submission-967139bec3b4fef1317b`
- Rendered script: `sha256:9ebe56b8f2c16ce7a51fd82aab7968eb3aa11dfb72deb7128dfe0c9d381a3751`

## Exactly-once execution

- One crash-safe submission intent was fsynced before `sbatch`.
- Exactly one `sbatch` call returned job `150145`.
- The script fixed placement with `--nodelist=itiger07`.
- Allocation: one CPU, 1 GiB memory, no GPU, one-minute limit.
- Slurm terminal state: `COMPLETED`, `ExitCode=0:0`, elapsed `00:00:01`.
- Application terminal record: `PASS`, exit code `0`.
- No OCI download, SIF materialization, NFD, CUDA, ONNX Runtime, or Qwen
  inference was attempted.

## Observed facts

```text
host=itiger07
uid=64102
user=tma1
gid=100
getent passwd 64102 -> tma1:*:64102:100:Tianxing Ma:/home/tma1:/bin/bash
apptainer path -> /usr/bin/apptainer
apptainer version -> 1.3.3-1.el9
```

The login node reported Apptainer `1.3.4-1.el9`, so runtime acceptance must bind
and report the compute-node version rather than assuming login/compute parity.

## Comparison with failed node

| Job | Node | UID lookup / Apptainer | Result |
|---|---|---|---|
| `149669` | `itiger02` | Apptainer: `unknown userid 64102` | `EXECUTED_FAIL` |
| `150145` | `itiger07` | UID resolves; Apptainer `1.3.3-1.el9` starts | `PASS` |

The next SIF materialization candidate may explicitly target `itiger07`, but it
must use a new candidate/run/submission identity and receive fresh explicit
authorization. Neither failed job `149669` nor preflight job `150145` may be
reused or relabeled as a materialization/GPU result.

## Evidence locations

- Local retained evidence:
  `results/spec110-itiger-qwen-live/runtime-release/spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0/itiger07-account-preflight/`
- Durable iTiger evidence:
  `/project/tma1/ndnsf-di/evidence/spec110/runtime-release-1a320e5/itiger07-preflight-spec110-submission-967139bec3b4fef1317b/`
- Slurm logs:
  `/project/tma1/ndnsf-di/evidence/spec110/runtime-release-1a320e5/slurm-itiger07-preflight-150145.{out,err}`
