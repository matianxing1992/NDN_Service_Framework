# B189 Material Consumer Evidence — 2026-09-19

## Scope

本单元把 T006 的 post-Selection consumer 接到 prepare 发布的 material manifest：Provider
先验证 root、manifest、payload receipt 与原始 graph/initializer identity，再只读取
template、被选择的 graph nodes 和显式 shared initializers，最后由现有 C++ worker/ORT
组装。material consumer 没有完整 source 或完整 initializer 的 fallback；legacy root
仍保留原有 source 路径。

## Static gate

官方只读 `review-agent` 对冻结快照 r6 返回 `STATIC_PASS`。审查范围包括 publisher
receipt、Repo root metadata、assembler 的精确 manifest 长度、digest/size/identity 绑定、
selected payload aggregate reservation、取消栅栏、worker provenance、legacy 分支和
C++ selector。快照为：

- diff: `.codex-tmp/spec189-material-consumer-review-r6/diff.patch`
  (`e51ae47977b5a251677fefc6e38bf4b2098bcb9104702e9ddf81df68b622dcc3`)
- selector: `.codex-tmp/spec189-material-consumer-review-r6/tests/integration-tests/ndnsf-di-native-assembly.t.cpp`
  (`be47624485d5619341561ca988ac41aa3ceaecc7eb70fbd32213ad0585612959`)

审查期间修复了三类边界：manifest receipt 增加认证的 `materialManifestBytes`；读取
manifest 使用 root 声明的精确长度并同时受 source/assembled ceiling 约束；每个选中
payload 在 fetch 前按 `manifestBytes + selected payload bytes` 做 overflow-safe 预算，
并在每次 fetch 前后检查取消/期限。

## Native validation

在 `build-spec189-b189-3-global-r3` 中执行：

```text
../waf build --targets=integration-tests -j1  PASS (3m37.037s)
../waf build --targets=unit-tests -j1  PASS (3m51.497s; after one scope-only compile fix)
```

构建只重用了现有全局依赖，重编 DI/worker/integration target；没有重编 ONNX Runtime
共享库。仓库根执行的 material selector：

```text
NDNSF_SPEC182_BIN_DIR=$PWD/build-spec189-b189-3-global-r3 \
  build-spec189-b189-3-global-r3/integration-tests \
  --run_test=Spec175NativeAssembly/Spec189MaterialConsumerBoundsSelectedPayloadFetches \
  --log_level=test_suite  PASS
```

该 selector 的正向 case 只向 fetcher 提供 manifest/payload，实际经过 C++ worker 和
ORT；完整 source name 从未读取。负向 case 将预算设为 manifest 加第一个选中 payload，
确认 manifest 与第一个 payload 各读取一次，所有后续 selected payload 以及完整 source
均未读取，并返回 `DI_CANONICAL_MATERIAL_BUDGET_EXCEEDED`。

同一命令从仓库根执行完整 `Spec175NativeAssembly` suite：8/8 cases PASS，包含旧 source
路径、1/2/4-provider ORT assembly、material-only 正负例和 YOLO merge regression。

publication/Repo receipt 单元也从仓库根执行并通过：`unit-tests --run_test='Spec189*'`
为 4/4 PASS，覆盖 canonical receipt reuse、取消回滚、legacy receipt 和 material
manifest publication/corruption。首次 unit build 暴露的 `RepoSourceProvider` 变量作用域
错误已在只读 r7 复审后修复；该失败日志保留在
`.codex-tmp/spec189-b189-material-unit-build-r1/build.log`，不计为产品行为失败。

原始日志：

- 首次工作目录错误（worker locator）：`.codex-tmp/spec189-b189-material-selector-r1/`
- selector 及 build-directory suite：`.codex-tmp/spec189-b189-material-selector-r2/`
- 根目录完整 suite：`.codex-tmp/spec189-b189-material-selector-r2/full-suite-root.log`
- unit build 与 Spec189 receipt selector：`.codex-tmp/spec189-b189-material-unit-build-r1/`

## Closure

状态为 `PARTIAL / LOCAL_MATERIAL_CONSUMER_PASS`。本单元关闭了 material-only C++
consumer 和选中材料预算边界，但没有证明生产 Core ACK/Selection ingress、真实 Qwen
两 Provider handoff、独立输出 oracle、资源 drain 或 MiniNDN qualification；T003、T005、
T006、T007、T009 继续保持 `PARTIAL`。
