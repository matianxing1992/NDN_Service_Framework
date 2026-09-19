# Spec189 B189-3 progress/heartbeat handoff evidence — 2026-09-19

## Scope

本单元把 post-Selection assembly 的真实 C++ 里程碑接到已认证的
`SelectionExecutionStatus`：Provider 只报告当前 assignment 的 operation、plan digest、
epoch/sequence 和 expiry；Requester 只接受同一 provider/service/request、同一 Selection
digest、同一 terminal-role operation 的严格递增状态。每个里程碑先经过 assembly deadline
检查，再由生产 assembler/Provider 发布；这不是 generic heartbeat，也不改变 ACK/Selection
授权边界。

## Static gate

官方只读 `review-agent` 对冻结 v6 快照返回 `STATIC_PASS`。快照只包含本单元 11 个文件，
未混入工作树其它修改：

- `.codex-tmp/spec189-r4-static-review-20260919-v6/changed.diff`
  SHA-256 `53eb4ab9750c5f8977e824d5c76e19f2071ebbbc5e6738a2a639d1b037ff893d`
- `.codex-tmp/spec189-r4-static-review-20260919-v6/files.sha256`

审查覆盖了 provider/service/request/digest 绑定、terminal-role operation ID、deadline、
epoch/sequence 单调性、in-flight retry 保留、同一 provider 多 role Selection 去重、
material assembler 的真实 milestone 位置，以及 same-provider multi-role C++ streamed
fixture。审查期间没有修改待审快照；跨 service 的同 provider 负例和真实 Qwen/MiniNDN
仍未覆盖。

## Native build and tests

使用已验证的 `build-spec189-b189-3-global-r3` 增量树；仅构建 NDNSF 自有 DI、examples 和
tests，未递归构建 NAC-ABE。6-core/12-GB 主机本轮使用 Waf `-j3`：

```text
../waf build --targets=unit-tests,integration-tests,ndnsf-distributed-inference -j3  PASS (6.758s)
../waf build --targets=di-native-assembly-worker -j3  PASS (0.301s)
```

C++ selectors 从仓库根执行；worker 目录显式设置为上述 build root：

```text
unit-tests --run_test=Spec175InvocationStreamLifecycle/StreamEventConsumerReordersDeduplicatesAndClosesOnMatchingResponse  PASS
unit-tests --run_test=Spec175InvocationStreamLifecycle  PASS (15/15)
integration-tests --run_test=Spec170NdnsfDiCoreFlow/PreconfiguredEnvironmentRunsSameProviderMultiRoleCollaboration  PASS
integration-tests --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse  PASS
integration-tests --run_test=Spec175NativeAssembly/AssignmentBoundRootSourceAndCachePath  PASS
```

这些用例覆盖了 progress 的 C++ 状态机、同 provider 多 role 的 non-terminal→terminal
stream、assembler 的 ROOT/SOURCE/WORKER 里程碑和已选材料的真实 worker 路径。第一次
完整 `Spec175NativeAssembly` 从仓库根运行时为 **8/9 PASS**：fixture 缺少
`metadata.canonicalSourceDigest`/`canonicalSourceBytes`，却期望
`DI_CANONICAL_SOURCE_NAME_MISSING`；assembler 正确先返回
`DI_CANONICAL_SOURCE_METADATA_MISSING`。只读复审确认这是 fixture 契约错误，test-only
JSON 修复通过第二次复审；integration target 以 `-j3` 在 26.040s 重建，修复后的
完整套件通过 **9/9**。初次和修复后的输出均保留在
`.codex-tmp/spec189-b189-3-r4-20260919/`；build-directory 首次运行缺少相对路径
`examples/trust-any.conf` 的启动失败也已保留，随后从仓库根复测。

## Five-lane result and closure

| Lane | Result |
| --- | --- |
| Static | `STATIC_PASS` (review snapshot v6) |
| Compile/link | `PASS` (`-j3`, affected DI/unit/integration/worker closure) |
| Runtime-test | PASS for corrected C++ assembly suite (9/9); no real Qwen/MiniNDN retry |
| Protocol/qualification | `UNOBSERVED`; no real Qwen/MiniNDN retry in this unit |
| Migration/evidence | `PARTIAL`; no cross-service negative and no two-Provider terminal/output proof |

本单元关闭的是受认证 progress/heartbeat 的本地生产接缝，不关闭 T003、T006、T007 或
T009。真实 Qwen run 仍必须沿 `prepare → Repo → ACK → Selection → placement-bound fetch/
assembly → terminal → drain` 重跑；不得用本地 selector 或静态通过替代资格结果。
