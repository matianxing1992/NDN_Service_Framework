# R10-B50 Provider Link Closure 2026-09-09

## First failure boundary

本批尝试把当前 `examples/DI_NativeProviderExecutable.cpp` 构建为独立 Provider
executable，使用已授权的 system-first toolchain 和主机资源策略 `-j2`。72 个编译任务全部
进入链接前完成，但 `di-native-provider` 在最终链接阶段退出 `1`。`ld` 报告
`NativeConversationJournal`、conversation wire/checkpoint、`NativeSelectionJson`、request
envelope、`NativePreSplitFirstPlacement::proposeRoles` 等符号未定义。

这说明 examples 的 `di_native_session_sources` 没有闭合当前 native Provider 的 source
closure；不是 Provider 协议结果、MiniNDN 结果或环境资格结果。原始失败输出保存在
`.codex-tmp/spec182-r10-b50-provider-build/build.log`（首次工具中断的部分输出）以及本次
直接构建的命令输出；没有启动 Provider 进程。

## Changed gate

静态检查沿 `examples/wscript` 的 target/source 注册和 unresolved symbols 反查定义文件，
确认缺失实体为：

- `NativeConversationJournal.cpp`
- `NativeConversationWire.cpp`
- `NativeAuthenticatedGrantClient.cpp`
- `NativeRequestEnvelope.cpp`
- `NativeSelectionJson.cpp`
- `NativeV3Placement.cpp`

重建后这些缺口已消失，但第二次链接又暴露了同一 source closure 中的两个遗漏：
`NativeSignedGrantRequest::sign`/`NativeArtifactGrantIssuer::issue`（由
`NativeArtifactPolicyAuthority.cpp` 提供）以及 `planNativeRequest`（由
`NativeRequestPlanner.cpp` 提供）。第二次命令仍在链接阶段退出 `1`，没有启动 Provider
进程；这两个定义文件已通过源码检索确认，必须在下一次重建前一并加入。

第三次链接在加入上述两个文件后继续发现 `NativeOfferAdmission::verify`、
`NativeCanonicalPreparationCatalog::bindStateContracts`、`NativePlanProjectionBuilder::build`、
`NativeGroupKeyAdmission` 和 `NativeGroupProjectionBuilder::build` 未定义。它们分别由
`NativeOfferAdmission.cpp`、`NativeCanonicalPreparationCatalog.cpp`、
`NativePlanProjectionBuilder.cpp`、`NativeGroupKeyAdmission.cpp` 和
`NativeGroupProjectionBuilder.cpp` 提供；该边界同样发生在链接阶段，未启动 Provider。

为闭合这些传递依赖，最终 source list 还显式加入
`NativeCanonicalArtifactPublisher.cpp`、`NativeCanonicalPreparationCatalog.cpp`、
`NativeCanonicalRolePreparer.cpp`、`NativeCatalogModelAdapter.cpp`、`NativeRequestCatalog.cpp`、
`NativeOfferAdmission.cpp`、`NativeObservedOfferV3.cpp`、`NativeGroupKeyAdmission.cpp`、
`NativeGroupProjectionBuilder.cpp` 和 `NativePlanProjectionBuilder.cpp`。

这些文件已加入共享 `di_native_session_sources`，使 `di-native-provider`、fault-provider
及相关 examples 使用同一 native source closure。修复后必须以同一 `-j2` 命令重建并核对
最终链接结果；若再出现未定义符号，保留新的首边界，不把编译完成计作 build PASS。

## Current status

`BUILD_BLOCKED` at link closure. No product task is closed by either failed attempt. Provider
cross-process transport、maintained caller/no-Python 和 T016 qualification remain open。

## Repaired build result

在登记上述失败边界后，`di_native_session_sources` 补齐了全部缺失 translation units。使用
同一 system-first `-j2` 命令重建，80/80 tasks 完成并成功链接：

```text
/usr/bin/time -f 'ELAPSED_SECONDS=%e' env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH \
  ./waf build --targets=di-native-provider -j2
```

Waf exit `0`，elapsed `39.34s`，产物为
`.codex-tmp/spec182-r4-b2/build/examples/di-native-provider`，SHA-256
`4be6b29acb10b29792757a23ce0ffc4f098f39e9cec2f0f47f4175d03bdae75a`。`ldd` 对关键
NDN/Boost/ONNX/NAC-ABE/OpenSSL 依赖均解析到实际路径，无 `not found`。Provider CLI 当前不
接受 `--help`（返回 usage/exit `2`），这只是既有命令行行为，不能作为业务或资格结果。

同一 source list 随后用于 `./waf build --targets=di-native-fault-provider -j2`，91/91 tasks
完成并成功链接，elapsed `255.25s`，产物 SHA-256 为
`94b94c8ca5be3b3eca9c0107213f39db35a83b34d90ae8b81421f7f1cce9b681`；`ldd` 未报告
`not found`。该回归只证明 fault-provider 的编译/链接闭包，不启动故障注入或网络请求。

## Closure decision

`CLOSED_FOR_VALIDATION` for the standalone Provider link/source-closure boundary;
`OPEN_FOR_NEXT_BATCH` for Provider `--serve`, independent requester/Provider transport, maintained
caller/no-Python and T016 qualification. The first three link failures remain part of this record;
the repaired build does not retroactively turn them into passes.
