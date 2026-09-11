# Spec183 Tiger runtime checkpoint — 2026-09-11

本检查点记录今天在同一 v23 base SIF + 外置 APP 分层组合上的真实结果。候选
接收、transport、Slurm、Apptainer、MiniNDN 进程和 collector 均按
`Experiments/TigerCluster/jobs/yolo/submit.py` 的 `prepare → local/submit →
collect --reconcile` 顺序执行；失败记录保留，不覆盖旧 receipt。

## Candidate and source change

Base SIF remains
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`.
The APP application manifest remains
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`.
The v54 harness manifest was
`sha256:c067cbb4e7587074cae8f70ec3db081071615ccf65740e912dc792401862525e`.
The v55 harness manifest is
`sha256:0d8f6ca4bce7032d2bcf0834a729c3f82149569a986dbdd99b31a6ce8db9f1c9`.

The source fix is collector-only: `runtime/yolo_negative.py` canonicalizes
labelled numeric NNI components emitted by ndn-cxx and accepts the two observed,
request-bound session forms (`requestId` and `requestId/attempt/1`). It also
binds the exact native failure using the same two session forms. Application
exit 134 remains accepted only for the registered negative user invocation; all
other finite worker exits still require zero. Focused regression:

```text
PYTHONPATH=Experiments/TigerCluster pytest -q \
  Experiments/TigerCluster/tests/test_yolo_negative_collection.py \
  Experiments/TigerCluster/tests/test_yolo_application.py \
  Experiments/TigerCluster/tests/test_yolo_runtime.py
120 passed in 6.43s
```

## Fresh PASS receipts

| Run | Slurm / host | Candidate digest | Verdict digest | Result |
| --- | --- | --- | --- | --- |
| `tiger-local-cpu-v54-r1` | local owner | `sha256:f7269bfbe3520fd13c279d362e51677df3f97993618ab201824e553a9e2f7a52` | `sha256:c6dc38bfd5b47dab418ce87ef5a231a985400d67b94ed5f7ba4a3f95aeec9e43` | `NORMAL_EXPERIMENT_PASS`, 2 requests, CPU graph, clean cleanup |
| `tiger-single-node-gpu-v54-r1` | `210402`, `itiger02` | `sha256:636a1510621e6c9cab11b1ef31e38b467d5d6d4b8105208d2ce66b232d13f5d5` | `sha256:7a07cbe3c62445f40e163dc7f8577a432ed2cecf56938e2be57a93ad7852655d` | `NORMAL_EXPERIMENT_PASS`, 1 warmup + 1 measured, model roles CUDA, Merge CPU, max abs error 0.00042724609375 |
| `tiger-two-node-gpu-v54-r1` | `210403`, `itiger02,itiger03` | `sha256:560cdb16402c13799d7d8d3ab5daac9cdb05c679a055ec8cf8400459429d591c` | `sha256:7b38c8328be2dc2e809428ca1db653744669745cf71d749f85d023a8204f35d9` | `NORMAL_EXPERIMENT_PASS`, 1 warmup + 3 measured, 4 roles, 9 dependency edges/request, two GPU identities, clean cleanup |
| `tiger-local-cpu-v55-r1` | local owner | `sha256:a7f74fc95c3bad83303932fb9f772b8d3bf9e3274ffe5eac459f2046af6f581e` | `sha256:5237a36b0b0a975c1ffc1216f4a4887c0391d5ddd48bd5dc649d3c13dbb1cf21` | `NORMAL_EXPERIMENT_PASS`, harness-fix rerun |
| `tiger-single-node-gpu-v55-r3` | `210440`, `itiger03` | `sha256:a76984461783c6c1631b99ea6eb219669d373e5e59361847a2321e0555828465` | `sha256:72f30c951d448b7ad4d5c56e0d9c3c3ac47eb87d1d2cbfa404b4e183879a9787` | `NORMAL_EXPERIMENT_PASS`, CUDA model roles, CPU Merge, clean cleanup |

## Retained failures and boundary findings

* `tiger-negative-dependency-v53-r19` / `210394` reached the native withheld
  edge but the collector first rejected ndn-cxx NNI numeric name encoding
  (`NEGATIVE_CUTPOINT_EDGE_IDENTITY`). This was fixed by canonicalizing only
  labelled numeric components.
* `tiger-negative-dependency-v54-r19` / `210411` ended Slurm `FAILED 78:0`.
  The User observation, one withheld DetectShard0→Merge edge and exact Merge
  native failure were present, but retained collection then rejected the native
  requestId session form (`NEGATIVE_CUTPOINT_BINDING`). No negative verdict was
  written, so this is not `EXPECTED_REJECTION_PASS`.
* `tiger-two-node-gpu-v55-r1` / `210441` and
  `tiger-two-node-gpu-v55-r2` / `210455` were both assigned to
  `itiger05,itiger06` and reached 900-second `TIMEOUT` before
  `collection-input.json` was retained. Their Slurm stdout files were empty and
  no SIF, CUDA, numerical or collector failure was observed. The repeated
  same-pair timeout is retained as a Tiger allocation/startup boundary, not
  classified as an application correctness failure.
* `tiger-two-node-gpu-v55-r4` / `210458` was cancelled after 20 seconds on the
  same node pair to release the allocation; its journal was reconciled as FAIL.
  It is not a qualification attempt.

Because v55 changed the sealed harness, v54 two-node evidence cannot satisfy the
v55 `twoNodeGpu` reuse gate. The v55 negative run was therefore correctly
rejected before Slurm with `GATE_RETAINED_EVIDENCE:twoNodeGpu`. T015 and T016
remain open pending one healthy v55 two-node PASS, followed by the negative
collector and a second unchanged normal allocation.

## Reproduction flow

1. Verify the profile dispatch plane and exact base/app/harness identities.
2. Prepare a new run ID; exit 78/`PREPARED` is expected and is not qualification.
3. For local validation, run `submit.py local`; for Tiger, run
   `submit.py submit`, wait for Slurm terminal state, then run remote
   `collect --reconcile` with the cluster operator interpreter.
4. Synchronize the retained run and remove group-write bits from evidence files
   before transport reuse; candidate and staging directories keep their own
   `0700`/read-only contracts.
5. Treat `NORMAL_EXPERIMENT_PASS` as a gate only when candidate content,
   harness digest, profile behavior, runtime versions, allocation, backend/GPU,
   numerical output and cleanup all match the prepared run.

The exact next action is to submit the unchanged v55 two-node profile when a
healthy node pair is available, then collect T015 and perform T016. No base SIF
rebuild is indicated by today's failures.

## 2026-09-11 v56 final qualification

The v56 candidate is the final immutable composition for this checkpoint. It
keeps the v23 base SIF (`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`,
3,586,351,104 bytes) and external read-only APP manifest
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`.
The sealed harness is `sha256:09d8bb4d237453acf1a7a33048363712c5bfdde87e8623a2be359f7eff969ed`
(3,663 bytes), with dispatch `sha256:d18cf044576875ac2aebc6d6b67a98439ac42ee37dd68510abcf8ff1bdd65545`,
inputs `sha256:acd62dd7c78323f89b2a47ac27348a4d672cb1907e3802816a4d650a31e00933`,
and runtime `sha256:16cbf7db11888d43e5d7e1431bd30b03928776660f62869f0050e6eb78c8fed3`.
The two-node, single-node and negative profile digests are respectively
`sha256:7ff6b810bd7981332828a2768266bac64e44748c5201e29927b6b76b7a274e5b`,
`sha256:71a1eb8bb98de3320ebb772ed514eb75bc72d0d1ee285541e8df58a7767f11d3`,
and `sha256:4f2447761437891531ffad04fccf1ba9e572f12ddb5906a92e87d81a53746bdb`.

| Run | Slurm / hosts | Candidate digest | Verdict digest | Result |
| --- | --- | --- | --- | --- |
| `tiger-local-cpu-v56-r1` | local owner | `sha256:f885f1b4c9020a71f06655bb702ea9a382e665b45aed3d7e93077386283dde63` | `sha256:2159969ea07e52265e3147f8c28b2afe3485a078648e21cfbd83a314b3c88e89` | `NORMAL_EXPERIMENT_PASS`; 2 requests, CPU graph, 9 edges/request, shape `[1,50,6]`, max abs error `0.0005340576171875`, cleanup closed |
| `tiger-single-node-gpu-v56-r1` | `210471` / `itiger02` | `sha256:f8d72b4e0d80ddc63ec7df5f02aa3066fa8b161716bc7d37a1afa9399ad29da1` | `sha256:43ad2d5729798ac4f03901dbab22e80c4db261e0d72e6d37457e2fb79036d032` | `NORMAL_EXPERIMENT_PASS`; 1 warmup + 1 measured, model roles CUDA, Merge CPU, GPU `GPU-0fab1e2a-dc0e-f3b0-62c0-f2dfab914341`, cleanup closed |
| `tiger-two-node-gpu-v56-r1` | `210472` / `itiger02,itiger03` | `sha256:f53f2dda60b5f3011de72715148c42460a67774ff0bbbde32562e680a03cd526` | `sha256:0972a0d69c4c866c45bf9c6228629746d3f0311b6fb74e8b5df61cfb95b4d03a` | `NORMAL_EXPERIMENT_PASS`; 1 warmup + 3 measured, four roles, 9 edges/request, CUDA model roles, CPU Merge, cleanup closed |
| `tiger-negative-dependency-v56-r1` | `210473` / `itiger02,itiger03` | `sha256:9bed0f0175b81ff53621ba781fca9be41c7788883d3b28e6db955051d208c6eb` | `sha256:15cbf0df6f48e685aab3c15fdf2f0f3be90402fccc5a1f3ed0e98e575fdbd76b` | `EXPECTED_REJECTION_PASS`; exact DetectShard0→Merge withholding, native `DEPENDENCY_DATA_MISSING`, no response/reselection, bounded cleanup |
| `tiger-two-node-gpu-v56-r2` | `210474` / `itiger02,itiger03` | `sha256:291652f18ec41f346a974086c7bd4740b234e15b4f9003d8edcc3e95aa43c185` | `sha256:b8513ddf693b74a22d6214d19d91fe3d55ba81ff1a9454a4a3e5386f686039d6` | `NORMAL_EXPERIMENT_PASS`; independent 1 warmup + 3 measured allocation, same profile/content/graph, new GPU UUIDs, cleanup closed |

The normal runs all use graph digest
`sha256:d8b40347e4cb60e7a0f74b3503d04816ba897a4e8f8733e9d59a38a8c9a65ed1`.
The first two-node allocation observed GPUs
`GPU-0fab1e2a-dc0e-f3b0-62c0-f2dfab914341` and
`GPU-acfab0d6-a493-a983-5d6a-6008c44adfd4`; the independent reuse allocation
observed `GPU-519e5825-d84a-8e55-670c-c53433c4a71c` and
`GPU-bcd15abe-d49a-9c07-d63e-acec4f7a8cbf`. Thus SC-003 has eight successful
normal requests across two allocations, and SC-004 has new run, allocation and
GPU identities with unchanged behavior inputs.

The v55 r20 collection boundary was caused by a native Merge reason wrapper
(`dependency wait failed: failed to fetch signed exact Data:`) that the collector
did not yet accept. The v56 harness and `runtime/yolo_negative.py` now accept
that exact wrapper only when request/session, role, edge and manifest identity
still match; the regression suite contains the wrapper case. This is a
harness/collector correction, so the unchanged base SIF was reused. A first v56
single-node submit attempted a full 3.6 GB upload because the remote candidate
root was absent; it was canceled before Slurm, then the root was hardlink-cloned
from v55 and only changed planes were synchronized. Evidence files initially
created with mode `0664` were normalized to `0644` before transport; content
hashes did not change.

This checkpoint closes T001–T017 and the required FR/SC gates. It makes no
performance-superiority claim. Raw run directories remain in project storage;
private keys, model files, SIF bytes and large logs are not tracked by Git.
