# iTiger SIF materialization evidence: job 149669

## Verdict

`EXECUTED_FAIL` — the authorized CPU-only Slurm materialization was submitted
exactly once, reached `itiger02`, and failed before OCI download because the
compute-node Apptainer process could not resolve the allocated user UID.

This is an iTiger compute-node account/NSS prerequisite failure. It is not an
OCI, Docker image, NFD, CUDA, ONNX Runtime, or Qwen inference result.

## Frozen identity

- Candidate: `spec110-c1-48b6832b59c1-dddb41e5c89c-6e76a5492a0c-d63ca16a4a3c-0cb3ef905bcc-fdc6531c6bae`
- Cell: `spec110-cell-c1f757c639938fde0caf`
- Run: `spec110-run-18edf65520acca142c4c`
- Submission: `spec110-submission-78936191eda59b501440`
- Rendered script: `sha256:f98cc02d0fc381a16501a9f992f5f1221c9585e84318b847751290446a2c4efa`
- Immutable OCI: `ghcr.io/matianxing1992/ndnsf-di@sha256:dddb41e5c89cc8f24fe1cdba250c0dd675f244a9f0cdd64a99bc34d48cd4cf2e`

## Submission and terminal state

- Crash-safe intent was fsynced before `sbatch`.
- Exactly one `sbatch` call returned job ID `149669`.
- Job name: `spec110-78936191eda59b50`.
- Slurm partition/account/QOS: `bigTiger` / `devs` / `normal`.
- Allocation: one `itiger02` node, 2 CPUs, 16 GiB, no GPU.
- Start/end: `2026-07-14T10:18:50` to `2026-07-14T10:18:51` local cluster time.
- Slurm state: `FAILED`; `scontrol` reports `ExitCode=255:0` and
  `Reason=NonZeroExitCode`.
- Application terminal record: `state=FAIL`, `exitCode=255`.
- Exact stderr:

  ```text
  FATAL:   Couldn't determine user account information: user: unknown userid 64102
  ```

## Boundary evidence

- The batch shell started on `itiger02` as Slurm user `tma1` / UID `64102`.
- It selected `/tmp/tma1/spec110-sif-149669.7Q9Qlm` as compute scratch.
- The first `apptainer version` invocation failed while writing environment
  evidence; `materialize-sif.sh` never began the OCI download.
- No `runtime.sif` or durable materialization record was promoted.
- The job-owned compute scratch directory was removed by the trap.
- No RTX 5000/GPU job was submitted, so T215 remains locked.
- The failed submission identity is frozen and MUST NOT be resubmitted.

The login node resolves `tma1:*:64102:100:...` through `getent`, while the
compute-node Apptainer process reported that UID 64102 was unknown. The next
admissible action is to have iTiger restore/confirm UID resolution on compute
nodes (or document a supported site workaround), then create a new candidate,
run, and submission identity with fresh explicit authorization.

## Evidence locations

- Local retained evidence:
  `results/spec110-itiger-qwen-live/runtime-release/spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0/itiger-cpu-materialization/`
- Durable remote partial evidence:
  `/project/tma1/ndnsf-di/evidence/spec110/runtime-release-1a320e5/.cpu-materialization-spec110-submission-78936191eda59b501440.partial/`
- Remote Slurm logs:
  `/project/tma1/ndnsf-di/evidence/spec110/runtime-release-1a320e5/slurm-cpu-materialize-149669.{out,err}`

## Administrator request

Ask iTiger administrators to verify that UID `64102` (`tma1`) is resolvable on
all `bigTiger` compute nodes, specifically `itiger02`, from a Slurm allocation,
and that site Apptainer 1.3.4 can execute for this account. Include job `149669`
and the exact fatal message above. No project or container change can honestly
convert this failed run into a PASS.
