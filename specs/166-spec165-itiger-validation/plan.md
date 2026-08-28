# Implementation Plan: Spec 166 iTiger External Validation

## Constitution Check

- Real NDNSF network and inference evidence runs under Slurm, never on a login
  node.
- The exact locally authorized image, model, workload, and negative results are
  immutable experiment inputs/evidence.
- MiniNDN remains the default local blocking gate; TigerCluster is the
  explicitly authorized external-validation layer.
- Failed measured submissions are retained and never silently retried.

## Sequence

1. Verify the new Spec 165 local aggregate and freeze all identities.
2. Export the exact Docker image to one temporary project archive.
3. Use a bounded CPU Slurm job to convert the archive to SIF; verify and
   promote the SIF, then remove only this run's validated temporary archive.
4. Stage the exact Qwen3 snapshot, ONNX stages, workload, policy, runtime
   manifest, and current application scripts with checksums.
5. Run a one-node RTX 5000 standalone ONNX CUDA exact-token reference.
6. Reuse accepted NFD TCP/UDP evidence for the same pinned nodes
   (`itiger07-09`, job `173264`), then submit one exact-once three-node run.
7. Monitor Slurm state and progress artifacts every 30–60 seconds; use a hard
   walltime only as the terminal automatic bound.
8. Validate JSONL, request lineage, GPU placement, CUDA provider, timing, and
   checksums; update Spec166 evidence without changing Spec160/165 history.

## Resource contract

- Partition/account/QoS: `bigTiger` / `devs` / `normal`.
- Nodes: exactly `itiger07,itiger08,itiger09`.
- GPU: `gpu:rtx_5000:1` per node.
- Tasks: one per node; four CPUs and 32 GiB memory per task/node.
- Standalone walltime: 10 minutes.
- Distributed walltime: 30 minutes.
- Durable root: `/project/tma1/ndnsf-di`.
- Runtime scratch: `$SLURM_TMPDIR`, otherwise job-unique `/tmp`.

## Security and reproducibility

No password, Duo token, private key, Hugging Face token, or bootstrap token is
stored in source or evidence. SIF/native libraries come only from the frozen
candidate. Model and source mounts are read-only. Failed and partial evidence
is retained under its unique submission identity. No automatic retry exists.

## Verification

- Local: CUDA provider-selection and execution-profile policy unit tests,
  exact sbatch environment validation, and complete Spec165 Gate A-D.
- Remote preflight: SIF checksum, `nvidia-smi`, ORT provider list, device UUID,
  scratch write, model/artifact checksums.
- Standalone: exact eight-token sequences for all eight workload rows plus one
  retained ONNX Runtime profile per stage. Each profile must pass the frozen
  CPU shape/control allowlist and required CUDA core-operation checks.
- Distributed: same evidence validator as Spec165 plus node/GPU/CUDA/stage
  transport evidence.

## Current Checkpoint

- Materialization job `181068`: `COMPLETED 0:0`; exact candidate image ID and
  promoted SIF SHA-256 verified.
- Frozen remote input bundle: staged and checksum-verified.
- Reuse-backed local Gate A-D run `20260731T155338Z-84787254` passed against
  current profile-policy v2 without copying the 5.2 GiB prepared artifacts.
- Standalone job `181085`, immutable `source-005`, completed `0:0` in 56 s.
  It produced 8/8 exact eight-token rows and three retained profile-policy-v2
  PASS profiles on `itiger07`.
- Seven three-node failures (`181086`, `181088`, `181089`, `181091`, `181092`,
  `181094`, `181095`) are preserved. They respectively exposed executable-mode,
  host/container path, sealed-image API drift, incomplete source packaging,
  bootstrap-token cleanup, missing ONNX timing/lineage evidence, and
  non-atomic mixed-language logging defects. No identity or source bundle was
  overwritten.
- Three-node job `181096`, immutable `source-012` manifest SHA-256
  `a7044a2332cad6a7e31a85d402257988d83310698b4b97428d5688745adc829a`,
  completed `0:0` in 5:02 on `itiger07-09`. The promoted analysis is PASS:
  8/8 exact generations, 64 unique token request IDs present on every Stage,
  192 CUDA executions with zero fallback, digest continuity across both Stage
  boundaries, and three distinct RTX 5000 UUIDs.
- Final evidence is
  `/project/tma1/ndnsf-di/evidence/spec166/multinode/spec166-multinode-qwen3-008`.
  Its checksum manifest passes and it retains neither plaintext bootstrap
  tokens nor private keys.
- After the final lineage and atomic-log fixes, local Gate A-D was rerun as
  `20260731T164038Z-d1540ad6` using the same checksum-verified prepared-run
  links. It passed all four gates and did not submit another TigerCluster job.
