# R11-B9-G11 Current Whole-Chain Static Audit

本批在 `729555fa`、`5528fa4e`、`ed9b7c9d` 之后重新核对当前源码，专门复查
R10-B82 审计中的生产调用链结论。静态检查使用 CodeGraph 追踪
`DI_NativeRequester`、`NativeInferenceClient::request`、`dispatchOperation`、
`beginCoreRequest`、`APPClient` native facade，以及当前 examples caller inventory；
随后用定向脚本确认入口接线和维护调用计数。

## Findings

- **SA-01 RESOLVED**：`examples/DI_NativeRequester.cpp` 现在读取可选
  `conversation` 配置，通过共享的 `nativeConversationCoordinatorFromConfig` 构造
  C++ coordinator，并把 continuation 放入 `NativeRequestOptions`。输出 checkpoint
  也由同一 coordinator 提交后写出。
- **SA-02 RESOLVED_FOR_CURRENT_COMPOSITION**：requester 配置拒绝
  `authority_private_key_file`、content key 等 authority-owned 字段；grant client
  使用 `issueThroughCore` 和 `publishThroughCore`。这证明当前 requester 不再持有
  authority signing/content key，但独立 authority service 的部署资格仍需 T014/T016。
- **SA-03 OPEN**：当前维护 examples 中仍有 16 个实际调用，分布在 5 个文件：
  `llm_pipeline/user.py` 8、`yolo_2x2/user.py` 4、`pytorch_eager_2x2/user.py` 2、
  `llama_server/user.py` 1、`yolo_split/user.py` 1。它们仍混用 compatibility 或
  automatic-planner API；native helper 存在不等于默认路由已切换，T013 与 legacy
  zero-use 继续 OPEN。
- **SA-04 OPEN**：当前 C++ unit/regression 与单个 PO-001 MiniNDN stream 子出口已
  通过，但完整 I01--I08/PO matrix、维护 caller、continuation/recovery 和
  no-Python zero-use 尚未形成同一资格运行。
- **SA-05 OPEN**：当前构建的动态依赖仍是 host-bound；exact-SIF、可复定位 ABI/RPATH
  及多机 closure 仍由 T014/T016 负责，不能由本地 `ldd -r` smoke 替代。
- **SA-06 MITIGATED_BUT_UNQUALIFIED**：request ID 使用每个 client 的 128-bit
  `processRequestOwnerScope()` 加单调计数器，已避免同一宿主上不同 client 的直接前缀
  冲突；跨进程、同 requester identity 的确定性唯一性仍没有独立 qualification，需在
  T016 运行中保留并核对。

## Verification

定向源码检查结果：`conversation_loader=True`、`authority_private_key_rejected=True`、
`issue_through_core=True`、`native_runtime_ctor=True`；维护 caller 计数为 5 个文件、
16 个调用。当前 C++ `*Spec182*/*` selector 已由 R11-B9-G10 记录为 260/260 cases、
7107/7107 assertions，fresh `-j3` build 由 R11-B9-G8 记录为 190/190 tasks、
57.934 s 且无持续 swap。

本批是当前源码静态收敛审查，不推进任何 parent task；T004--T017 的状态保持原值。
下一出口应先完成 maintained caller 的逐入口 native migration 与 no-Python 反例，
再进入 T014 → T015 → T016 → T017。
