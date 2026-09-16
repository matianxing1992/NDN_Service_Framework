# YOLO base CPU inference

## Scope

用户要求本机 YOLO 推理与 base → NDNSF → APP 分层。当前可执行镜像仅为 `base.sif`，本记录首先验证该 base 内真实 YOLO26n CPU 推理；不冒充最终 NDNSF+APP、Request/ACK/Selection/Response 或 MiniNDN 通过。现有交付脚本的外置 APP 同时携带 NDNSF framework 和 DI application，尚未独立发布 NDNSF 中间层。

## Preparation

复用 `.codex-tmp/spec180-yolo-candidate-current` 的 canonical graph、external weights 和独立 PyTorch oracle；固定 P3 fixture 来自仓库。首次手动摘要预检误把 `oracle.outputDigest` 当作裸 float payload 摘要，导致断言失败；查看 exporter `_oracle_manifest` 确认它实际绑定完整 NPY 文件，完整文件 SHA-256 `ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175` 与 manifest 相等。该失败是预检命令错误，没有开始推理，不是模型失败。

原始运行目录 `.codex-tmp/yolo-sif-smoke-20260915/`；C++ oracle、预处理和推理在 `Experiments/TigerCluster/tests/yolo-cpu-smoke.cpp`。固定快照 `review-r1/` 交由官方只读 review-agent 审查。五 lane 为 ORT C++ consumer、容器内编译调用、独立 oracle/fixture、模型与镜像身份、日志与资格边界。

## Result

官方 review-agent r1 要求补齐输入身份绑定和硬超时；r2 对冻结的 C++ 文件与 `run-yolo-cpu-smoke.py` 返回 `STATIC_PASS / YOLO_CPU_SMOKE_COMPOSITION_PASS`。入口固定核对 graph、weights、fixture、oracle 全文件摘要，要求显式 SIF SHA-256；运行前后重验，容器只读挂载输入。子进程 120 秒 timeout，外层 180 秒进程组终止和回收。

实际命令：

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-yolo-cpu-smoke.py \
  --base-sif Experiments/TigerCluster/images/base-repair-20260915/run-r2/base.sif \
  --expected-sif-sha256 7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c \
  --model-root .codex-tmp/spec180-yolo-candidate-current \
  --output .codex-tmp/yolo-sif-smoke-20260915/run-r1
```

结果 **PASS / YOLO_CPU_MODEL_SMOKE_ONLY**，exit 0；镜像内 GCC 9.4、binutils 2.34、ORT 1.20.0，CPU 4 intra-op threads / 1 inter-op thread。C++ 编译命令、实际 ldd、source/binary/input hashes 均在 `run-r1/record.json` 与 `run.log` 中。

| Measurement | Result |
| --- | --- |
| ORT session creation | 188.091 ms |
| Inference 0 | 182.073 ms |
| Inference 1 | 105.685 ms |
| Inference 2 | 91.0535 ms |
| Canonical rows / each run | 50 |
| Maximum absolute error / each run | 0.000366211 |
| Oracle tolerance | `abs(delta) <= 1e-3 + 1e-4 * abs(reference)` |
| C++ executable SHA-256 | `7a8af8e281b7593dab1dc90d2a77d70e6acb2cd4b8c770f41ff85f293ee7e041` |

这是三次诊断观测，不是统计性能结论；4×4 固定色块输入也不能评价真实照片识别准确率。没有修改生产 C++ 或重建 base，没有生成最终 NDNSF/APP 或上传 Tiger。

输入身份负例 `reject-sif-r1/record.json`：传入全零预期镜像 hash，返回 1 和 `INPUT_HASH_MISMATCH`；记录中没有 container command，也未创建二进制，确认在启动容器前拒绝。复核两个已执行源文件与 `review-r2` 逐字节一致。

## Retrospective and remaining

- static：输入 hash/timeout 在执行前补齐；复审无控制性缺陷。
- compile-link：真实容器内单 C++ consumer 编译成功；没有编译宿主对象供容器使用。
- runtime-test：三次实际 YOLO 推理与独立参考一致。
- unobserved：完整 NDNSF+APP build、请求链和 MiniNDN；当前结果不能关闭 T001/T003。
- Context Mode active health 因三个 Spec 源 hash 过期失败；采用实际仓库文档、源码与本轮原始记录为依据。CodeGraph 通用查询命中许多历史副本，转为当前 exporter 和路径定向源码核对。
