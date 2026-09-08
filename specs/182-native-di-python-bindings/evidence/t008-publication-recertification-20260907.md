# T008 Publication Recertification

## Scope and Source Review

基线 `19d6edea`。T008-A/T004-A 保持 PARTIAL；实现发布后的 root 验证与封存衔接，
不把端口测试当成真实 Core publication/requester 或 ONNX assembly 验收。

维护中的 YOLO `adapters/yolo/adapter.py:445` 发布 canonical source、initializer 和
业务 root，`app_sdk/placement.py:4326` 用最终 model_manifest_digest 构造新
CertifiedOnnxAssemblyRecipe。业务 root payload 的摘要不同于 Core 传输 manifest。
原生先前要求 publication manifest 与 inspection manifest 永远相等，无法保留此顺序。

实现由 ArtifactPort 返回原始业务 root 字节和独立的稳定工件名字。preparation 验证
ACTIVE/schema、原始字节摘要、模型、profile、inspection source object digest/字节数、
可选 initializer object digest/字节数及 package manifest；不接受仅更新摘要的 DTO。
bindPublishedRoles 复用 canonicalNativeOnnxRecipeJson，仅更新最终 manifest/recipe，
不改变候选工件、角色、Provider 或设备。sealer 先独立验证原 proposal，再检查更新后的
recipe 是否仍可由同一 admitted offer 执行；旧 exact-reuse 证明不足时拒绝。

源码审查覆盖 NativeRequestPreparation、NativePlanSealer、既有 V3 feasibility、
native ONNX recipe encoder 和 SDK oracle author。检测重点：篡改原始 root 字节、
跨模型/profile/source/package/initializer、缺稳定名字、无 root 偷换 manifest、
发布后旧 exact-reuse 证明失效。原 source name 前置要求仍存在，实际本地 source owner、
Core 加密 publisher 和 requester Begin/Commit 接线由后续 T008/T010 继续完成。

## Validation Plan

DTO ABI 改动使用全新 `.codex-tmp/spec182-t008-publication-recertification-r1/build`。
冻结系统 compiler/binutils、Boost、NAC-ABE、SVS、ONNX/ORT/Rust archive，单一 -j4
构建 unit-tests。先由真实 SDK CertifiedOnnxAssemblyRecipe/PlacementPlanCoreV3 生成
CPU、多 rank 发布后摘要 oracle，并保留 loaded-device 旧 recipe 拒绝场景。
只运行准备、V3 placement/sealer、admission/planning/grant/client 相邻定向单元测试。
不启动全量集成、MiniNDN、SIF 或 Tiger；Python 扩展未重建，不作为新 ABI 证据。

## Result

执行入口（configure 的实际依赖路径见 r1/preflight.log 与 build/c4che/_cache.py）：

```bash
python3 tests/fixtures/spec182/author-placement-v3-oracle.py
PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin \
  python3 ./waf -o .codex-tmp/spec182-t008-publication-recertification-r1/build \
  build --targets=unit-tests -j4 -v
.codex-tmp/spec182-t008-publication-recertification-r1/build/unit-tests \
  --run_test=Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient \
  --report_level=detailed --log_level=message
```

r1 author/configure PASS（configure 5.295s），新 ABI -j4 build PASS（302.273s）。
首次 focused exit 201，57/58 cases PASS：fixture 用前导 / 的 role 拼接稳定工件名，
产生 //，被既有名字校验拒绝；尚未完成发布后 sealer oracle。修正测试的名字构造，
产品校验保持不变，使用 r2 独立日志目录重验。r1 原始日志保留。

r2 build PASS（24.664s），focused exit 0，58/58 cases、710/710 assertions PASS。
SDK oracle 的七个 sealing 场景包含发布后 CPU、多 rank 的 recipe/core 摘要匹配，
以及旧 loaded-device recipe 的 exact-reuse 拒绝；既有四场景保持通过。
root 字节、模型/profile/source/package/initializer 替换、缺稳定名及无 root 更新均拒绝。
r1 ldd.log 无缺失依赖。vmstat start/middle/end 后续样本 so=0，si 少量非零；
仅为短采样，不作为全程峰值或加速比。r2 design-validation.json errors=[]，
git diff --check PASS。Context Mode project health PASS，源码仍以仓库/CodeGraph 为准。

PARTIAL：NativeInferenceClient::dispatchOperation 仍明确失败
NATIVE_REQUEST_PIPELINE_NOT_READY，当前端口结果校验不代表端口已实际发布。
下一步以 ServiceUser.hpp:618/620 的原生加密发布接入具体 owner，保留 Core I/O
线程调度、deadline/cancellation 与对象寿命；本地 source inspection 也须返回真实
字节身份和 canonical node mapping。Python 扩展及正式请求/网络资格未运行。
