# Spec186 standalone YOLO GPU reference

**Captured:** 2026-09-12 (America/Chicago)
**Run:** `spec186-standalone-yolov8n-r2`
**Node:** `itiger02`
**Qualification:** `STANDALONE_REFERENCE_ONLY`

This is a same-model GPU reference for separating ONNX Runtime/CUDA and model
issues from the pending NDNSF-DI composition. It is not a Spec186 candidate
run: the SIF is the existing Spec183 v56 image and its labels identify the
older Spec174 source seal `sha256:9766e37fcedd176a4316e795142db3106287b85cb0102f367b267d136f4d0127`.

| Field | Observation |
| --- | --- |
| SIF | `/project/tma1/ndnsf-di/candidates/spec183-v56-20260911/planes/runtime/base-runtime-controller-version-j4-v23.sif` |
| SIF SHA-256 | recorded in the remote candidate directory; not promoted to Spec186 |
| Apptainer | compute node `/usr/bin/apptainer`, `1.5.3-1.el9`; executed with `--nv --cleanenv --containall` |
| ONNX Runtime | `1.20.0`; configured providers `CUDAExecutionProvider`, `CPUExecutionProvider` |
| Model | `/project/tma1/ndnsf-di/models/spec186/yolov8n.onnx`, SHA-256 `4d2f387218a088c40a19dd8b41067ba729504f37d258f133377221b148e2f373` |
| Input | `images`, `[1,3,640,640]`, `tensor(float)`; zero tensor |
| Workload | 1 warmup + 1 measured request |
| CUDA observation | ORT profile contains 350 `CUDAExecutionProvider`/CUDA events |
| Measured time | 4.262384 ms |
| Output | `[1,84,8400]`, SHA-256 `995ef4d64f724c130552df73e27f413f5017667bc45effcb8cd2114469452d75` |
| Durable receipt | `/project/tma1/ndnsf-di/results/spec186-standalone-yolov8n-r2/standalone-reference.json`, SHA-256 `47799faca32718c5602559b92c01bda9cd4b44a20bbbda7af3045767487750f7` |
| ORT profile | `/project/tma1/ndnsf-di/results/spec186-standalone-yolov8n-r2/ort-profile_2026-09-13_03-37-45.json`, SHA-256 `fa46cafda57a677fe2a73d940d5740a8df1329bf11085d44eebc9b4be3ac996a` |

The output shape is the raw YOLOv8n ONNX output and is intentionally not the
Spec186 `[1,50,6]` post-processing oracle. This reference therefore supports
the GPU/model/backend substrate only; T009.b still requires the exact
source-sealed base SIF, application bundle, four native roles, CPU Merge,
NDN protocol completion, independent oracle and cleanup.

The first attempt (`spec186-standalone-yolov8n`) reached ORT execution but
failed while writing its receipt because `--cleanenv` removed the `SIF`
environment variable (`KeyError: 'SIF'`). It remains preserved as a separate
failure; the r2 run uses fixed in-container paths.
