# B189 Materialization and Worker Evidence — 2026-09-19

## Scope

本记录收口 T006 的一个可独立验证边界：native canonical materialization 将已验证的
ONNX source/initializer 交给有界 worker，worker 通过分段 pipe frame 接收材料并返回
assembly metadata。它不把组件 selector 结果提升为真实 Repo、Provider、ACK/Selection
或 MiniNDN 资格。

## Static gate

官方只读 `review-agent` 对冻结 diff 的最终结论为 `STATIC_PASS`：

- `NativeOnnxAssemblyWorker.cpp`：固定 frame header、metadata/model/initializer 顺序、
  partial write、空 initializer、总 payload 上限、分块 reserve，以及 process-group
  cleanup 均有明确出口；2 GB/EPIPE/cancel/crash 的运行时边界仍需 runtime evidence。
- `NativeCanonicalOnnxAssembler.cpp`：source/initializer fetch 后立即建立 plaintext
  guards；canonical source scrubber 覆盖 worker、startup 和 exception 路径；digest、
  size、service/name/role/request/attempt 绑定保持在最终 owner 中。

审查期间曾发现并修复两个问题：initializer guard 绑定 moved-from 容器，以及 material
fetch 日志先于 plaintext guard 建立。修复后对受影响范围复审通过。

## Focused native verification

使用 `build-spec189-b189-3-global-r3`，先以 `-j1` 重建受影响 DI、worker 和 unit
targets，构建耗时 `1m50.493s`；随后补齐 worker protocol helper targets，构建耗时
`2.093s`。没有重建 Core、Repo 或 Python binding。

```text
Spec182NativeAssembly:       5/5, No errors detected
NativePreparationContext:    3/3, No errors detected
Spec189RepoPublication:      4/4, No errors detected
Spec182OnnxWorkerProtocol:  30/30, No errors detected
```

worker subprocess selector 使用了显式 `NDNSF_SPEC182_BIN_DIR`，指向本次构建的
worker/tool binaries；第一次未设置该目录的运行只暴露了 fixture discovery boundary，
未计入通过数。

## Limits

该证据仍是 native component/fixture boundary。尚未证明实际 protected Repo ingress、
真实 ACK/Selection、两个 Provider 的 Qwen 0.6B 执行、独立 C++ output oracle、取消后
资源 drain 或第二次完整 run。因此 T006、T007、T009 保持 `PARTIAL`，不能写入
`QWEN_TWO_PROVIDER_PASS`。
