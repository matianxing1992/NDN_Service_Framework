# TigerCluster Remote Checkpoint — 2026-07-31

## Frozen identities

- Local authorization run: `20260731T064155Z-e3a1011e`
- Candidate image ID:
  `sha256:dcef2858c060ba0ba01903dd57ca5d3e844d1c1f9b500ed59f5dba9dd753ac47`
- Workload digest:
  `sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9`
- Model digest:
  `sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a`

## Completed materialization

- Slurm job: `181068`
- Terminal state: `COMPLETED 0:0`
- Candidate archive SHA-256:
  `27e8baf5dfacd6e0e7ec3842f1175989ffeee8cdc55b42c6bd43047b6c461663`
- Promoted SIF SHA-256:
  `e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`
- Release:
  `/project/tma1/ndnsf-di/releases/spec166-dcef2858c060`
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/materialization/spec166-materialize-dcef2858c060-001`
- Verification: release SHA-256 and every evidence checksum passed.
- Cleanup: only this run's validated temporary Docker archive and checksum
  sidecar were deleted; the promoted SIF and evidence remain.

## Frozen remote inputs

- Path: `/project/tma1/ndnsf-di/inputs/spec166`
- Size at staging: 4.3 GiB
- Contents: complete pinned Qwen3 snapshot, three pinned ONNX stages, workload,
  generation campaign, original/remote policy, and service manifest.
- Verification: every file passed
  `/project/tma1/ndnsf-di/inputs/spec166/checksums.sha256`.

## Preserved standalone failure

- Slurm job: `181070`
- Submission ID: `spec166-standalone-qwen3-001`
- Terminal state: `FAILED 1:0`
- Node/GPU: `itiger07`, NVIDIA RTX 5000 Ada Generation,
  UUID `GPU-25e44831-503d-7227-29f7-d2346fbb1d80`, 32760 MiB.
- Durable failure evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/.spec166-standalone-qwen3-001.partial`
- Failure occurred before ONNX Runtime session/model load:
  `ModuleNotFoundError: No module named 'ndnsf_distributed_inference'`.
- Root cause: the standalone job used container system `python3`; the package
  is installed in the candidate's `/opt/venv`.
- No automatic retry occurred. The three-node job was not submitted.

## Corrective state, not remote proof

- The job now invokes `/opt/venv/bin/python`.
- The immutable corrected bundle was staged at
  `/project/tma1/ndnsf-di/jobs/spec166/source-002`; its checksum manifest
  SHA-256 is
  `18b3a6eae6062a32325147ba933408903a0890ed60526e32ece39602b40a925d`.
- Submission `spec166-standalone-qwen3-002`, Slurm job `181071`, ran once on
  `itiger07` and ended `FAILED 1:0` before ONNX Runtime session creation.
- Durable second failure evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/.spec166-standalone-qwen3-002.partial`.
- Exact second failure:
  `ModuleNotFoundError: No module named 'ndnsf_distributed_inference'`.
- Root cause: `/opt/venv/bin/python` was correct, but the sbatch environment
  exposed only `/source/llm_pipeline`; the application package is under
  `/opt/ndnsf-app/python`.
- No automatic third submission occurred. The three-node job remains
  unsubmitted.
- Future submissions require a versioned `SPEC166_SOURCE_ROOT`; the failed
  jobs' `source` and `source-002` bundles must not be overwritten.
- The local sbatch source now specifies the exact application environment:
  `PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python`.
- Exact local candidate interpreter/environment import check:
  `SPEC166_EXACT_SBATCH_ENV_IMPORT_PASS`. The probe derives both the
  interpreter and `PYTHONPATH` from the actual sbatch source.
- Local job-source regression checks: `4/4` pass.
- This correction was subsequently frozen as `source-003` and exercised only
  by the new submission identity recorded below.
- These local checks prove the import-path correction only. They do not establish
  CUDA inference success or authorize claiming T005/T006 complete.

## Preserved CUDA partitioning failure

- Immutable source:
  `/project/tma1/ndnsf-di/jobs/spec166/source-003`
- Source checksum-manifest SHA-256:
  `a3f715803ab606b97c9e2219e345b7ea396070aaf6e4ea6f0ba524433c4ed090`
- Difference from `source-002`: only
  `standalone-gpu-reference.sbatch`, changing from SHA-256
  `94c77420782108ff61918042f07c511c51d8495ed05b31c7d2e2f1085996b894`
  to
  `7169ef49e7caba013d691efc7b295cdbe1d0a83f1d4880bdd0ac0314c07163f3`.
- Slurm job: `181072`
- Submission ID: `spec166-standalone-qwen3-003`
- Terminal state: `FAILED 1:0`
- Runtime: 32 seconds on `itiger07`
- GPU: NVIDIA RTX 5000 Ada Generation,
  UUID `GPU-25e44831-503d-7227-29f7-d2346fbb1d80`, 32760 MiB.
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/.spec166-standalone-qwen3-003.partial`
- The candidate interpreter and both application import paths succeeded.
  Source checksums and GPU visibility also succeeded.
- Failure occurred during the first ONNX Runtime session initialization:
  `This session contains graph nodes that are assigned to the default CPU EP,
  but fallback to CPU EP has been explicitly disabled by the user.`
- The preceding runtime warning reported one CUDA `Memcpy` node. No session,
  placement row, generated token, or latency result was produced.
- Local read-only graph inventory confirms the frozen stages contain many
  dynamic shape/control operators (`Shape`, `Gather`, `Range`, `Where`,
  `ConstantOfShape`, and related operators). This is consistent with the need
  to audit execution-provider partitioning, but does not identify which node
  ONNX Runtime assigned to CPU.
- No automatic retry occurred. No three-node job was submitted. Any future
  source must use immutable `source-004` after the FR-005 policy is resolved.
- ARS experiment-result record:
  `evidence/experiment-result-181072.md`.

## Final accepted standalone gate

- Reuse-backed local Gate A-D identity:
  `20260731T155338Z-84787254`; all four gates PASS.
- Job `181084` / `source-004` is retained as `FAILED 1:0`: its 8/8 exact rows
  proved execution correctness, while profile-policy v1 incorrectly rejected
  bounded CPU shape/control metadata and fused CUDA normalization.
- Profile-policy v2 requires bounded `int64`/`bool` evidence for every allowed
  CPU shape/control event and CUDA evidence for matmul, softmax, and
  normalization semantic groups.
- Job `181085` / `spec166-standalone-qwen3-005`, immutable `source-005`,
  completed `0:0` in 56 seconds on `itiger07`.
- Source manifest SHA-256:
  `f2bd6c0887ddfd90356a7671117b1f11ad71d7919da1bc3d00cecab36b8fdc55`.
- Result: 8/8 exact eight-token rows and three profile-policy-v2 PASS profiles.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/spec166-standalone-qwen3-005`.

## Three-node deployment closure

Seven unique failures were retained before closure:

- `181086`: non-executable rank wrapper;
- `181088`: host wrapper used a container-only source path;
- `181089`: sealed SIF API did not accept the required `request_id`;
- `181091`: checksum-bound API overlay omitted compatibility package data;
- `181092`: successful requests followed by bootstrap-token residue at the
  post-run security gate;
- `181094`: successful CUDA requests without required ONNX timing/lineage
  evidence fields;
- `181095`: correct lineage fields corrupted by interleaved native/Python
  stdout writes.

Each correction was published under a new immutable source and submission
identity. The original `.partial` evidence directories were not rewritten.

Job `181096` / `spec166-multinode-qwen3-008` used immutable `source-012` and
completed `0:0` in 5:02 on `itiger07-09`. Source manifest SHA-256:
`a7044a2332cad6a7e31a85d402257988d83310698b4b97428d5688745adc829a`.
The final analyzer accepted:

- three distinct RTX 5000 GPU UUIDs;
- 8/8 exact eight-token generations;
- 64 unique request IDs shared by all three Stages;
- 192/192 timing records on `cuda:0` with zero CPU fallback;
- per-request digest equality across Stage 0→1 and Stage 1→2;
- nonempty Data names for both intermediate publications;
- checksum-valid evidence with no retained bootstrap token or private key.

Promoted evidence:
`/project/tma1/ndnsf-di/evidence/spec166/multinode/spec166-multinode-qwen3-008`.
The complete chronology and descriptive latency distribution are recorded in
`evidence/experiment-results-181085-181096.md`.

Final-source local Gate A-D revalidation `20260731T164038Z-d1540ad6` then
passed using the required prepared-run reuse path. The local runner did not
submit another TigerCluster job.
