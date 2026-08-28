# Tasks: Spec 166 iTiger External Validation

- [x] T001 Verify VPN, SSH, live Slurm/GRES, project storage, accepted NFD
  network probe, and the current Spec165 authorization aggregate.
- [x] T002 Add fail-closed ONNX Runtime CUDA selection and preserve the CPU
  default; pass focused tests and rerun Spec165 Gate A-D.
- [x] T003 Materialize and checksum the exact candidate SIF under project
  storage without retaining an unnecessary OCI duplicate.
- [x] T004 Stage and checksum the exact Qwen3 snapshot, ONNX stages, workload,
  policy, runtime manifest, and application source bundle.
- [x] T005 Submit and validate one exact-once standalone RTX 5000 reference.
  Failed jobs `181070`–`181072` and `181084` remain immutable. After
  profile-policy v2 and local Gate A-D run `20260731T155338Z-84787254` passed,
  job `181085` completed `0:0`: 8/8 exact eight-token rows, three CUDA-first
  stage profiles, no profile-policy violations, and checksum-valid evidence.
- [x] T006 Submit and monitor one exact-once three-node RTX 5000 NDNSF-DI
  workload without automatic retry. Jobs `181086`, `181088`, `181089`,
  `181091`, `181092`, `181094`, and `181095` are retained under distinct source
  and submission identities. Each exposed a different deployment or evidence
  defect. Job `181096` (`spec166-multinode-qwen3-008`) completed `0:0` on
  `itiger07-09` with one distinct RTX 5000 GPU per Stage.
- [x] T007 Validate and promote evidence, record negative results unchanged,
  and produce the final post-run audit. Final evidence contains 8/8 exact
  generations, 192/192 CUDA Stage executions, 64 unique application-owned
  request IDs shared by all Stages, verified per-request digest continuity,
  full answers and latency distributions, checksum integrity, and no retained
  bootstrap token or private key.
  Final-source post-run Gate A-D identity `20260731T164038Z-d1540ad6` also
  passed using the required prepared-run reuse path.
