# R10-B64 Native Config Contract Repair

## Scope and Baseline

本批次基于本地 `Experimental` 当前源码，处理 R10-B63 静态审计中 native-config Qwen
生成契约的两个可执行缺口。没有修改 skill，没有运行网络、MiniNDN 或 Tiger 资格流程。

## Changes

- `APPClient.configure_native_requester_from_config` 现在只接受
  `TOKEN_DIAGNOSTIC` 与 `TOKEN_STREAMING`，并在 catalog 已解析后拒绝 Qwen adapter 的
  diagnostic runtime。Qwen native catalog 因而不会等到 planner 才暴露 generation/stream
  冲突。
- `nativeRequestRuntimeFromJson` 使用相同的允许值集合，保持直接 C++ 配置调用方与
  Python facade 一致。
- Qwen full-generation caller 在 native config 与 `--diagnostic-token-loop` 同时出现时
  立即 fail-closed；native final 只接受 `NDNSF-DI-FINAL-V1` 的 `tokenIds`。
- 原有 observer mapping 类型门保持不变，并增加 malformed JSON list 回归，证明
  terminal observer 通知不能掩盖 caller-edge validation failure。

## Review and Validation

静态检查覆盖 `APPClient` 配置解析、Qwen full/diagnostic 分支、C++ runtime parser、
pybind-facing tests 与 Spec182 planner test；没有发现新的跨层接线缺口。测试结果：

- `python3 tests/python/test_spec182_native_bindings.py`：16/16 PASS。
- `python3 -m py_compile NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py examples/python/NDNSF-DistributedInference/llm_pipeline/user.py tests/python/test_spec182_native_bindings.py`：PASS。
- `./waf -o build-nac182 build --targets=unit-tests -j4`：188/188 tasks，link PASS。
- `Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift`：PASS。
- `Spec182NativePlanning,Spec182NativeInferenceClient,Spec182ClientState`：49 cases PASS。
- 并行 `vmstat 1 12` 在首个样本后 `si`/`so` 为 0；主机仍有历史 swap 分配，未把它写成性能资格结论。

## Closure Boundary

本批次关闭 F-02/F-04 的本地配置/调用方契约边界，并为已存在的 F-03 mapping guard
补充回归。它没有证明真实 Provider、跨进程 stream、conversation owner、request-id
映射、maintained caller migration、legacy zero-use 或 T016 qualification；T010、T013、
T015、T016 继续保持 `PARTIAL`/`UNQUALIFIED`。

`Closure decision: OPEN_FOR_NEXT_BATCH`。下一批冻结 native/application request identity
映射并执行真实 native-config Qwen requester → Core → Provider stream。
