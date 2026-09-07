# T006-D Protected Provider Activation — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182OnnxActivation/*)。承载 suite 建于
[tests/unit-tests/di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)
（沿用 T006-B/T006-C 决策）。实现：provider 侧装配激活路径从 legacy
in-process helper（OA01）切换到同库 bounded worker transport
（OA02 `runNativeOnnxAssemblyWorkerAt` / OA03 `runNativeOnnxAssemblyWorkerMain`，
T006-C 冻结协议），签名前父端保留全部自我核验（result hash / io 名与数量 /
manifest digest），worker 位置显式经 `workerLocation` option 注入并由
DI_NATIVE_ASSEMBLY_WORKER_LOCATION_MISSING 早检查兜底。

## 执行命令与结果

```
./waf -o build-nac182 build -j4        # rc=0；最后一次增量 26.8s（t006d-build7.log）
./build-nac182/unit-tests --run_test=Spec182OnnxActivation --log_level=test_suite
#   rc=0；9/9 全绿，*** No errors detected（t006d-activation-final.log）
./build-nac182/unit-tests --run_test=Spec182OnnxWorkerProtocol/SubprocessChainRejectionPropagatesItsOwnCode
#   rc=0；1/1 全绿（t006d-final-focus.log / t006d-fix-focus.log）
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归 852 cases，No errors detected
#   （t006d-unit-full.log）
```

raw 输出见 `.codex-tmp/t006d-*.log`（build/build2..7、probe、poison2、
activation-focus[失败版]、fix-focus、activation-final、unit-full）。

## 卡 Steps 对照

- **替换 helper 调用为同库 worker，移除旧文件 IPC/Python 参数**：
  [NativeCanonicalOnnxAssembler.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp)
  的装配入口由 OA01（in-process `assembleNativeCertifiedOnnxModel`）切换为
  OA02 `runNativeOnnxAssemblyWorkerAt`；签名保留 canonical chain 同一
  `prepareNativeCanonicalOnnxRole` 路径，不再透传 Python 参数/旧 IPC 句柄。
  OA01 在
  [NativeOnnxRecipeAssembler.hpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp)
  标注 legacy/suite-only（Spec182OnnxIdentity suite 继续使用）。
- **worker location / pinned hash / secret lease 保留**：workerLocation
  option 新增（hpp）并经现有调用点喂入——integration
  [ndnsf-di-native-assembly.t.cpp](../../../tests/integration-tests/ndnsf-di-native-assembly.t.cpp)
  3 处 site、[DI_NativeProviderExecutable.cpp](../../../examples/DI_NativeProviderExecutable.cpp)
  resolve+pinned（env → 自身目录 → build-nac182 → build 候选，`sha256:` 前缀
  完整比较）；`registerNativeOnnxWorkerLocation` registry default 路径保留；
  parent transport 每次 spawn 前 re-probe；empty sha 跳过、非空不匹配 ->
  DI_NATIVE_ONNX_WORKER_PREFLIGHT。无 workerLocation 时在 sign 前以
  DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING 拒绝（早检查，置于 grant
  runtime 检查之前）。
- **父端重新核验结果 hash/权限后才激活**：transport ok 路径上父端继续校验
  result modelDigest、io 名/count 与固定 manifest；root/source digest
  mismatch、certified graph poison、cancel、timeout、pinned-hash tamper、
  worker crash-by-signal 均在 entry gate / finalize 前被拒绝且
  checkNothingActivated 证明未发生任何签名/发布（9 cases 逐一对齐）。

## 关键发现与修正

1. **worker child catch off-by-one（T006-C 冻结代码缺陷，T006-D 激活覆盖暴露）**：
   `ActivationRejectsCertifiedGraphPoisonThroughWorker` 首次运行失败——真实
   worker 子进程把 frozen `reject-identity-digest` 行（recipe.graphDigest
   毒化）返回成 DI_NATIVE_ONNX_WORKER_INTERNAL 而非链拒绝码
   DI_NATIVE_ONNX_RECIPE。根因：child catch 的
   `what.compare(0, 14, "DI_NATIVE_ONNX_")` 中族字面量为 15 字节，三参
   compare 把整个字面量当右值，14 字节子串永不可能等于 15 字节字面量 →
   S1-S7 全部链拒绝被错标 INTERNAL，parent transport 原样转播了错误 reason
   码。修复：bound 14→15。排查用 in-process replay probe（metadata builder
   → validator → chain）与 DEBUG-TEMP 插桩锁定根因，隔离验证 compare 语义
   后修复。新增 frozen-lock case
   `Spec182OnnxWorkerProtocol/SubprocessChainRejectionPropagatesItsOwnCode`：
   真实 worker + frozen reject 行，断言 parent 收到精确 DI_NATIVE_ONNX_RECIPE。
   已登记 case-manifest 并在 [docs/failure-log.md](../../../docs/failure-log.md)
   （2026-09-07）记录完整诊断。
2. **完整回归 852 cases（排除 StreamFacade）零失败**：排除族同 T006-B/C
   记录（TPM-PIB missing private key + NFD 环境性失败）。

## 文件

- [NativeCanonicalOnnxAssembler.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp)（workerLocation option + DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING）
- [NativeCanonicalOnnxAssembler.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp)（OA01→OA02 切换、parent 自我核验保留、requireActiveAssembly-before-sign）
- [NativeOnnxRecipeAssembler.hpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp)（OA01 legacy/suite-only doc）
- [NativeOnnxAssemblyWorker.cpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp)（child catch 14→15 修复；该文件 T006-C 冻结产物）
- [di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)（Spec182OnnxActivation 9 cases + regression lock）
- [assembly-parity-driver.cpp](../../../tests/fixtures/spec181/assembly-parity-driver.cpp)、[DI_NativeProviderExecutable.cpp](../../../examples/DI_NativeProviderExecutable.cpp)、[ndnsf-di-native-assembly.t.cpp](../../../tests/integration-tests/ndnsf-di-native-assembly.t.cpp)（workerLocation 注入点）
- [tests/wscript](../../../tests/wscript)、[examples/wscript](../../../examples/wscript)（worker TU 闭包：spec181-assembly-parity + di_native_onnx_assembly_sources）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)（T006-D 9 named cases + regression-lock entry）
- [docs/failure-log.md](../../../docs/failure-log.md)（2026-09-07：14-vs-15 off-by-one 全诊断）

残余风险：真实 provider 端到端激活（固定 manifest 冷装配 → 父端核验 →
sign → Selection 发布）与 crash/timeout 场景的进程级收尾按卡约定延至
T016；T006-D 各 negative case 已用 checkNothingActivated 锁定"未激活即无
签名"不变量，Selection-after 真实冷装配 case 属 T016 executeOwner。
