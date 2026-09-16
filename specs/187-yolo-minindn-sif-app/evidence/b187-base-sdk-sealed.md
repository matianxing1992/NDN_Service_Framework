# B187 Base SDK Sealed Release

## Result

2026-09-15：`PASS / BASE_DEPENDENCY_SDK_AND_YOLO_CPU_SMOKE`。最终 SIF 已实际验证并永久封存；不是 NDNSF candidate、MiniNDN、Tiger、GPU 或 QWEN 资格。

- SHA-256: `8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9`
- Size: `4087824384` bytes
- Directory: `/home/tianxing/NDN/ndnsf-artifacts/base-sif/8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9/`
- Parent: `7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`，旧镜像保留。
- Apptainer: `1.5.3`，binary SHA-256 `2cbfdcbc53a0a1eb56a1327cc42c4cfbdb48beaa03b9a26547df9e4556d3b673`。

## Actual Validation

| Check | Result | Durable evidence inside sealed directory |
| --- | --- | --- |
| Final SIF base smoke | PASS：NDN-CXX C++ consumer、ORT、NumPy 1.26.4 三个私有库、NFD 版本 | `evidence/final-base-sdk-test.log` |
| Final SIF SDK | PASS：4278 项文件摘要、dpkg 身份、C++ compile/link/run、ONNX checker/shape inference/full protobuf、NAC/SVS/NDNSD 符号与 ELF、Rust、GTK/GStreamer、Python ABI 和锁定包 | 同上；`dependency-sdk.json` |
| Corrupt SDK library | 预期 rc=1，`SDK_ARTIFACT_CHANGED:/opt/ndn-base/lib/libnac-abe.so`；只读覆盖，不改 SIF | `evidence/negative-sdk-library.log`、`corrupt-library.fixture` |
| Real YOLO CPU | 3 次 PASS，50 rows，max absolute error `0.000366211`；约 68.7/62.6/63.9 ms | `evidence/yolo-cpu/record.json`、`run.log`、C++ source/binary |
| Packaging regression | 46 passed，5.59s | `evidence/consumer-sdk-tests-r3.log` |
| Sealed files | `sha256sum -c SHA256SUMS` exit 0；移动后 `apptainer test` 再次 exit 0 | `SHA256SUMS`；原始 `.codex-tmp/spec187-two-layer-20260915/sealed-files-verify.log`、`sealed-base-retest.log` |

## Build And Review Trace

官方只读 review-agent：SDK r4 `STATIC_PASS / B187-BASE-SDK_COMPOSITION_PASS`；r5 增量 include 修复 `STATIC_PASS`。实际 r1 先编完外部库，在 SDK probe 缺 NAC include 子目录处失败；未覆盖原始 FAIL。r5 修复只增加 `/opt/ndn-base/include/nac-abe`，真实 rootfs probe 通过后，补齐等价 runtime metadata 并由 Apptainer sandbox→SIF 封装，没有重复编译依赖。最终 SIF 再执行全部 probe，而非沿用 rootfs PASS。

验收脚本 SHA-256 `37569747cf6a1c2a3902d530678684dfdabe09c8f64c90ff0778567724f059c4`；SDK manifest SHA-256 `fe85e2cc6d87586261bbfb803a61e498e8b976d3b1337964553bcfc1b4361f22`。主会话核对 r5 快照与实测脚本一致；consumer r1 快照清单也全部一致。封存 `build-inputs/` 保留首轮输入；`scripts/` 保存修正后脚本，`evidence/` 保存首轮失败及续建记录；`release-record.json` 明确续建来源与验收边界。

漏检回顾：static 阶段覆盖依赖接口/调用、loader、Python 与直接负例；compile-link 实测发现 NAC probe include 缺口，已修复复验；脚本测试发现旧 handoff 断言未随分层变化更新，修正并复审后 46 passed。NDNSF consumer 实际 build/runtime-test 仍 unobserved。

## Retention And Next Step

目录与文件已去除写权限，不入 Git，不进入临时清理；删除需单独明确授权。禁止原地覆盖，新增依赖产生新镜像和新摘要。AGENTS、技能及 base 文档已登记；`development-handoff.lock.json` 已绑定新 base 和 `maxBuildJobs=4`。

下一步消费此固定 base 构建 NDNSF 层；本轮未上传 Tiger 或启动 Slurm。模型/私钥仍外置，MiniNDN 拓扑编排及 namespace 权限由实验 harness 提供，本记录没有声称完成该请求链。
