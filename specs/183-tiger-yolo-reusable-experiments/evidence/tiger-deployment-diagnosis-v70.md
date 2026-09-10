# TigerCluster deployment diagnosis: APP v35 checkpoint

**Updated 2026-09-10.** This checkpoint records the APP v35 rebuild, the
single-node and first two-node normal Tiger gates, and the retained negative
failure after the first v34 retry exposed a backend selection defect. Local
MiniNDN/local CPU evidence remains separate from Tiger qualification.

## Candidate identity

| Item | Frozen value |
| --- | --- |
| Base SIF | `base-runtime-controller-version-j4-v22.sif`, 3901079552 bytes, `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5` |
| APP source | revision `e57273d9e769386c11ff744a8d7f907210cbee25`, source-seal file `sha256:77309970b505fc9d305f02176b803610d4402bfdcf553a98f8cf102e7590ec2b` |
| APP manifest | `sha256:7c8c994fda2bf20aaf322407e9c6b8a4d1f93a050f6d7782986cd4053fdeb91b`, buildKey `2649c5560533fff259b5d5336e924dd1717b4994f5a885bc9c98ae53a6f81065` |
| Plan planes | v50 inputs/runtime/dispatch; content IDs inputs `sha256:487c6a36019c0e92264ac38e07f2a87fcf319d12720994e1eb7688cfb719e980`, runtime `sha256:2504d0e27d179f920ba438999f2f619ac8b476a61aee64ae29651fef635dd6fa`, dispatch `sha256:98df6d49144a2845d8c9dab3b05629228dec4d38aa27e8d68698e8af2cb5c9ee` |
| Profile | `yolo-two-node-controller-v42.json`, raw SHA `sha256:537dc9ce35d30477ae80bba52af52d44476ed944be6cf4e2e2f31cd8b887fde4`, `startupSeconds=300`, shared host/local-cpu gates |

The base SIF canonical path was found to have drifted from the required SHA.
It was restored from the immutable stable copy before APP work. Because no base
library or ABI changed, the correction did not trigger a base rebuild; only the
external application and dependent candidate planes were regenerated.

## Today’s evidence

| Run / action | Result | Meaning |
| --- | --- | --- |
| Full build attempts | `integration-tests -j4` stopped at assembler `.sleb028`; bounded unit aggregate hit GCC9 `ggc_set_mark` ICE. The focused backend regression compiled and passed. | Host toolchain failure is recorded separately; it is not evidence that the APP or Tiger runtime failed. |
| APP v35 build | PASS in the exact v22 SIF with a single build job and existing cache; three application/native targets rebuilt and their hashes were recorded in the manifest. | Confirms app-only rebuild and immutable base reuse. |
| MiniNDN Y-B `minindn-local-20260910-v97-v35-yb` | `T010_DONE`, return code 0. | Real local process-boundary normal graph, not Tiger evidence. |
| MiniNDN Y-N `minindn-local-20260910-v98-v35-yn` | All registered subcases passed and cleanup was clean. | Local authorization/dependency negative coverage, not Tiger admission evidence. |
| Host gate `host-gate-v35-r2` | PASS; `host-minindn-v35.json`, 4510 bytes, `sha256:e4ad9acba7cab6ec1d548b8dfb14be94c550f4a8986575b6bacf1d7d8b45fa6d`. | Corrected archive now contains a per-run `public/preparation.json`; its source seal is copied into the candidate root so transport can verify the same bytes. The first archive was rejected for missing execution binding. |
| MiniNDN v99 | FAIL before execution with `GATE_HOST_SOURCE_BINDING`. | Reusing the v34 native manifest with an APP v35 source seal is correctly rejected. |
| MiniNDN v100 / shared `tiger-local-cpu-v35` | PASS; two requests, `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.0005340576171875`, nine dependency edges and clean cleanup. Receipt: 225893 bytes, `sha256:ba2f14ba2670b8bb69059edb4bb555f10a31577f3298ecce78e0a428a6ad09a0`. | Closes the candidate-bound local CPU promotion gate only. |
| Tiger job `210331` (APP v34) | FAIL after GPU probe and four-provider readiness. CUDA model roles and CPU Merge advertised the expected backends and ACK resources were non-empty, but the ORT profile contained only CPU providers; `DI_RUNTIME_CUDA_REQUIRED` stopped the request before numerical output. | The defect was backend propagation from `RoleAssemblySpec` into the ORT runner, not SIF transport, GPU visibility, or Tiger scheduling. |
| `210334` / `tiger-single-node-gpu-v35-r3` | PASS (historical v35 retry) | Earlier startup budget; retained single-node success remains valid component history. |
| `210340` / `tiger-single-node-gpu-v35-r7` | PASS; `collect --reconcile` returned `NORMAL_EXPERIMENT_PASS`, 2 requests (1 warmup + 1 measured), Slurm `itiger02` exit 0, verdict 230981 bytes, `sha256:86af42c1f7f79319abc32714a659e70ba8b247b094ad67517dd29329eb98d248`. | Current T013 gate: GPU UUID `GPU-254d5117-9a30-dfd1-5c38-51d196853e8b`, CUDA model roles, CPU Merge, cross-role dependency Data, numeric terminal response and clean shutdown. |
| `210341` / `tiger-two-node-gpu-v35-r5` | PASS; `collect --reconcile` returned `NORMAL_EXPERIMENT_PASS`, 4 requests (1 warmup + 3 measured), Slurm `itiger02`/`itiger03` exit 0, verdict 464725 bytes, `sha256:e25ee17d3e6564ebaaed7e53017f23a0565a81ddafd0a30ffb4de4a54fca4c82`. | Closes T014 first normal two-node gate: four roles, CUDA model execution + CPU Merge, 9 dependency edges/request, shape `[1,50,6]`, max absolute error `0.00042724609375`, and clean cleanup. |
| `210342` / `tiger-two-node-gpu-v35-neg1` | FAIL; Slurm `FAILED 1:0`, elapsed 4:31. Selection and two native withheld records were retained, but User supervisor timed out after `59.99460293306038` seconds; official collect returned `COLLECTION_INPUT_MISSING`. | T015 remains blocked. Completion budget reserves cleanup from the 90 s permission+request budget, leaving only 60 s for a 60 s negative observer; the graph also has two distinct DetectShard0→Merge edges while the collector requires one. |

## Fix and acceptance rule

APP v35 changes the ORT runner resolver so an explicit
`RoleAssemblySpec.backend=onnxruntime-cuda` selects CUDA when legacy metadata
does not contain an execution-provider field; explicit metadata still wins, and
CPU remains the fallback for `onnxruntime-cpu`. A focused regression covers this
metadata-omission case. The fresh Tiger run must show
`CUDAExecutionProvider` for BackboneNeck/DetectShard0/DetectShard1, CPU for
Merge, a numerical warmup and measured response, and clean collector evidence.
Provider readiness, CUDA UUID visibility, a partial withheld marker, or a local
CPU PASS alone cannot close T013/T014/T015.

## Reproducible flow

1. Run `check` to verify profile structure, hashes, modes, shared output parent,
   and candidate-bound gate identities.
2. Run `prepare` with a fresh run id; retain the preparation digest and public
   receipt. Exit 78 means frozen but not qualified.
3. Run `local` against the exact SIF and read-only APP. Require two requests,
   independent oracle, dependency evidence, and clean cleanup.
4. Publish the host receipt and candidate planes; run `submit --plan-transport`
   to enumerate the exact files, then verify the receiver has the shared project
   root, immutable base SIF, and no stale active journal.
5. Submit one fresh GPU run. On the allocation node, recheck SIF/app/plane
   hashes, Apptainer, HOME/scratch, NFD socket, GPU UUID and CUDA library
   injection before starting providers.
6. Require provider-owned non-empty CUDA resource rows, three CUDA model roles,
   CPU Merge, and the ORT profile. Execute one warmup and one measured request.
7. Run `collect --reconcile`; retain node/GPU, backend, numeric, dependency,
   exit-code, and cleanup receipts. Any first failure remains attached to its run
   and is never repaired in place.
8. After T013, run one normal two-node allocation and reconcile it before the
   negative case. For the negative case, ensure the completion budget covers
   permission wait, bounded observation and shutdown, and bind the suppression
   to one complete logical edge. Only after `EXPECTED_REJECTION_PASS` may a
   second normal two-node allocation run for reuse.

The current v35 candidate has **single-node and first normal two-node PASS**;
overall reusable qualification remains blocked by T015 and the unrun T016.
