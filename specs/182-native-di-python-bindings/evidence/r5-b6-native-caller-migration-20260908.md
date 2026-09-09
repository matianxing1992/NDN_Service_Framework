# R5-B6 Qwen and Streaming Native Caller Migration

## Status

`STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL`。

本批把维护中的 Qwen 普通请求接到 operator-pinned native requester configuration，并让固定
tiny streaming harness 转发该配置后在参数解析阶段拒绝不兼容 runtime。`APPClient` 只做文件
读取和 native owner composition；catalog、grant、runtime、admission、split 和 placement
仍由 C++ owner 持有。conversation continuation 尚未有 `NativeConversationCoordinator`
配置出口，因此入口明确 fail-closed，不调用 Python conversation coordinator。真正的 native
streaming execution、YOLO user、Provider migration、真实 Core/Provider 请求和 T016 不属于
本批完成范围。

## Static review

按官方 `$review-agent` 的只读 defect-first 方法检查完整差异、真实维护入口、harness 参数
转发、native binding 和构建注册：

- `configure_native_requester_from_config` 只组合 operator 配置和文件字节，所有语义摘要、
  身份、epoch、保护、budget、state mapping 与 admission 校验继续交给 native owner；没有
  新增 Python planner 或 silent fallback。
- `request_native_payload` 使用 catalog 暴露的 `model_ref`/splitter 和
  `NativePreSplitFirstPlacement`，把 application options 与 generation/stream DTO 一起交给
  `NativeInferenceClient`。
- Qwen 普通分支在 maintained user 中先选择 native route；conversation 缺少 owner 时抛出
  明确错误。`--native-requester-config` 与 automatic planning 互斥，且非 Qwen runtime 在
  参数解析阶段拒绝，避免 Tiny 路径误用 native Qwen contract。
- Qwen harness 和固定 streaming harness 只转发配置参数，不复制 native composition；后者
  仍固定 tiny runtime，因此转发配置只产生明确拒绝，不宣称 native streaming 已接通。
  Provider 入口没有被本批改动。

静态复核发现并修正了两个边界：operator contract 允许的绝对路径不能被错误拒绝；native
semantic `ValueError` 不能被配置文件的结构化错误捕获而吞掉。修正后未发现新的可执行缺陷。

## Verification

所有命令在仓库根目录执行，使用 system-first compiler/binutils 和本机默认 `-j4`：

```text
/usr/bin/python3 -m py_compile \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  examples/python/NDNSF-DistributedInference/llm_pipeline/user.py \
  Experiments/NDNSF_DI_QwenAckDriven_Minindn.py \
  Experiments/NDNSF_DI_StreamedGeneration_Minindn.py \
  tests/python/test_spec182_legacy_exclusion.py
-> exit 0

git diff --check
-> exit 0

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference /usr/bin/python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_spec182_native_bindings.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
-> 28 passed in 1.24s
```

Native option construction/import was also checked with the candidate extension; pybind represents
the generation ID as a string and stream generation IDs as an integer list, matching the C++ DTO
bindings. The shared target was rebuilt after the route guard was added:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf build --targets=ndnsf-distributed-inference -j4
-> exit 0; 5.419s incremental build
```

The extension source-closure rebuild, `ldd`, `nm`, and candidate dependency checks remain recorded
by R5-B6A. No network, MiniNDN, callback-lifetime, conversation continuation, SIF, or Tiger run was
performed here.

## Coverage matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry / callers | covered for Qwen user and harness config forwarding; gap for native streaming execution, YOLO and Provider | `llm_pipeline/user.py`, `NDNSF_DI_QwenAckDriven_Minindn.py`, `NDNSF_DI_StreamedGeneration_Minindn.py` |
| implementation and wire | covered for config composition, native payload, generation and stream DTOs; gap for native streaming executor and conversation owner | `APPClient.configure_native_requester_from_config`, `request_native_payload`, `_native_qwen_options` |
| test / harness / oracle | covered for route/source gates, non-Qwen fail-closed guard, DTO construction and Python compatibility/binding tests; gap for real Core/Provider and callback delivery | 28 focused pytest cases and option import probe |
| build / source closure | covered for shared DI target; extension closure inherited from R5-B6A | system-first Waf `-j4`, `py_compile`, `ldd`/`nm` evidence |
| migration / evidence | PARTIAL | this record and task row; T013-A/T013-B/T016 and conversation owner remain open |

## Remaining boundary

R5-B6 is not complete until the native conversation continuation owner is configured or split into
a separately accepted batch, maintained YOLO and Provider callers are migrated, and real Core/
Provider behavior is exercised. The explicit fail-closed branch is a safety boundary, not a
conversation qualification result.
