# B187 C++ large-data publisher regression

## Scope

本记录只覆盖当前工作区 `ServiceUser::publishEncryptedLargeData` 的 C++
分段发布边界。它验证签名后的分段、`FinalBlockId`、NDN 单包大小、IMS
重组和 AES-GCM 明文 oracle；不代表 Qwen、MiniNDN、SIF/APP 或 Tiger
qualification 已完成。

## Static review

审查 agent 对本轮不可变范围（`ServiceUser.hpp`、`ServiceUser.cpp` 和
`generic-dynamic-api-prepared.t.cpp`）返回 `STATIC_PASS`。审查确认测试使用
签名专用 LocalMock 钩子，不启动没有 authority 的 NAC-ABE bootstrap；测试事件
泵使用负 timeout，不会因没有后续事件永久阻塞。

## Compile and runtime evidence

- 临时注册的 `spec187-large-data-publisher` C++ selector 以 `-j4` 构建成功，
  用时 `1m46.201s`；构建后 `tests/wscript` 已恢复。
- selector 运行退出码为 `0`，单个测试用例 `LargeDataPublicationEmitsBoundedFinalizedSegments`
  的 `28/28` assertions 通过；原始日志为
  `.codex-tmp/spec187-large-data-publisher-20260916-r3.log`。
- 测试使用 20,000-byte synthetic plaintext，观察到至少三个连续 segment，
  每段 content 不超过 7,000 bytes、wire 不超过 8,800 bytes，签名验证、
  `FinalBlockId`、envelope 解码、AAD 绑定和 AES-GCM 明文重组均通过。
- 本次构建的 selector SHA-256 为
  `8a9d8213e5d044015c7275818a8636a708469191e4524e3c32d228b0817dd3fc`。
- 复用同一当前 `libndn-service-framework.so` 的 framework-closure 动态回归退出码为
  `0`，`26/26` cases、`204/204` assertions 通过；原始日志为
  `.codex-tmp/spec187-large-seg-framework-closure-20260916-r2.log`。

## Boundary

先前调用完整 `useSigningKeyChainForTest` 的重试在 NAC 公共参数获取处等待并
失败，原始日志 `.codex-tmp/spec187-large-data-publisher-20260916-r2.log`
保留；这不是分段实现的协议结果。修正后的 signing-only fixture 已通过，但
全量 `unit-tests`/`integration-tests` 仍受现有 ONNX C++ 头文件配置缺失阻断，
该缺口未被本 selector 绕过或伪造为全量 PASS。Qwen 真实模型、当前源码 MiniNDN
负路径、SIF/APP 和 Tiger 仍未执行；Spec187 T001/T002/T003/T007 保持
`PARTIAL`，T004 保持 `WAITING_EXTERNAL_INPUT`。
