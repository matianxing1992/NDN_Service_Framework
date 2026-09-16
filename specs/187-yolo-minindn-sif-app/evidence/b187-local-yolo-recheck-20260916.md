# B187 local YOLO MiniNDN recheck

2026-09-16 本轮只重放本机 MiniNDN Y-A，不启动 SIF、Apptainer、Slurm 或
TigerCluster。既有 `results/spec187-local-yolo-r27/subcase-result.json` 和
`results/spec187-local-yolo-r28/subcase-result.json` 仍为
`status=PASS`、`boundary=TERMINAL_RESPONSE`、`reason=TERMINAL_RESPONSE_VERIFIED`；
它们对应的持久记录见 [b187-local-yolo.md](b187-local-yolo.md)。

为了确认该结果可重放，使用同一 candidate-bound config/input、现有
`build-spec187-local-nac-r1` 二进制和独立状态/输出目录进行以下尝试：

| Run | First boundary | Protocol reached | Raw record |
| --- | --- | --- | --- |
| r29 | `REQUEST_ENVELOPE_KEY_UNAVAILABLE`（新 key 路径不存在） | no | `.codex-tmp/spec187-local-yolo-r29.log` |
| r30 | `SPEC187_NATIVE_REQUEST_OUTPUT_INVALID`（仍引用旧 output path） | no | runner terminal output |
| r31 | `Mininet must run as root` | no | `.codex-tmp/spec187-local-yolo-r31.log` |
| r32 | `STATE_ROOT_OWNER_MISMATCH`（root runner 使用用户目录） | no | `.codex-tmp/spec187-local-yolo-r32.log` |
| r33 | `CASE_RUNTIME_NETWORK_START_FAILED:IndexError:list index out of range` | no | `.codex-tmp/spec187-local-yolo-r33.log` |
| r34 | same MiniNDN startup boundary | no | `.codex-tmp/spec187-local-yolo-r34.log` |
| r35 | same MiniNDN startup boundary | no | `.codex-tmp/spec187-local-yolo-r35.log` |
| r38 | maintained-chain check with temporary parser shim | terminal response | `.codex-tmp/spec187-local-yolo-r38.log` |
| r39 | maintained runner after in-repo compatibility fix | terminal response | `.codex-tmp/spec187-local-yolo-r39.log` |
| r40 | `OUTPUT_ROOT_MISSING` before MiniNDN | no | `.codex-tmp/spec187-local-yolo-r40.log` |
| r41 | `SPEC187_NATIVE_REQUEST_OUTPUT_INVALID`, then `LOCAL_NATIVE_BUILD_REJECTED:PROVIDER_LINKAGE_CHANGED` | no | `.codex-tmp/spec187-local-yolo-r41.log` |
| r42 | Controller abort: `corrupted size vs. prev_size` | control phase only | `.codex-tmp/spec187-local-yolo-r42.log`, `results/spec187-local-yolo-r42/controller.log` |

r33–r35 的最小诊断将边界定位到旧 MiniNDN
`/home/tianxing/NDN/mini-ndn/minindn/util.py:78` 的 `popenGetEnv()`：
其 `var.split('=')[1]` 对节点环境解析不健壮。直接用健壮解析替换该第三方
函数后，MiniNDN `ndn.start()` 和 NFD `AppManager` 可以启动；本轮没有修改
该第三方代码，也没有把这些启动失败计作 NDNSF/YOLO 协议结果。

兼容层随后收回维护脚本 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`，
不修改外部 MiniNDN checkout；新增回归测试覆盖 `util.popenGetEnv` 和实际
`application.getPopen` 两条入口，并确认 `KEY=a=b` 的后缀不丢失。静态复审
返回 `STATIC_PASS`，指定 Python 检查 3/3 通过；整份旧测试文件另有一个既有
夹具失败（`initialize_keychains` 传入无 `net` 属性的哑对象），与本修复无关。

r39 使用维护脚本、现有 `build-spec187-local-nac-r1`、同一 candidate-bound
config/input 及新的 root-owned 状态/输出目录完成本机 MiniNDN Y-A：
`SPEC180_CASE_RESULT status=PASS case=Y-A`，C++ selector 写出
`SPEC187_NATIVE_REQUEST_PASS`，`subcase-result.json` 为
`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，native result 为
7,267 bytes；Controller、Repo、Provider、User 子进程均已清理。该结果是
host MiniNDN local PASS，不是 SIF、Apptainer 或 Tiger qualification。

r40–r42 是刷新本机 native receipt 后的独立重放边界：r40 保留输出根目录为空的
门禁，r41 保留输出路径和 provider linkage 身份门禁；按同一系统优先库路径重新
生成 receipt 并得到 `SPEC180_NATIVE_IDENTITY_OK` 后，r42 在 MiniNDN 控制阶段仍
于 Controller 证书注册边界以 `corrupted size vs. prev_size` 终止。该错误发生在
Controller/绑定启动边界，未到 ACK、Selection 或 Provider 执行，不能计为 YOLO
失败或 PASS；r39 的历史 PASS 继续保留，当前源码的可重放资格待最小化 native
崩溃诊断和复测。

最小化复现进一步确认该边界不依赖 YOLO 输入：在相同 native 绑定环境下，单独构造
`ServiceController` 可以返回 `CONSTRUCTED`，但显式删除对象时稳定返回 `SIGSEGV`；
`gdb` 顶层位于 `std::_Rb_tree<..., ndn::security::Certificate>` 析构路径。该
结果只证明 Controller/绑定 native 生命周期存在可复现崩溃，尚未归因到具体库或
修复；没有用 `os._exit`、强制杀进程或放宽身份门替代正常析构验收。

## 2026-09-16 C++ segmented request regression

为复核 r21 的单 Data 阻断，在当前源码加入了一个 C++ production-chain
回归用例 `RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput`。
request-scoped 输入基础名现在在 User 与 Provider 两端都以
`.appendVersion(attempt)` 结束，满足 ndn-cxx `SegmentFetcher` 的
`prefix/version/segment` 名称契约；Provider 仍按每段独立的 request/segment AAD
验证后才组装并调用服务 handler。

不可变静态审查快照为
`.codex-tmp/review-segmented-input-20260916-r3/`，changes SHA256 为
`1ca6d5017d0ffd0d8990bbd23dfb033fcc7cb1122432c0ca49af5042c210e020`，官方
`review-agent` 返回 `STATIC_PASS`，无 P0–P3。五 lane 覆盖 production callers、
implementation/wire、test/oracle、既有 `tests/wscript` source closure 和
evidence/migration；恶意缺段、乱序、错误 FinalBlock 与超时反例仍未运行。

使用既有 `build-spec187-local-nac-r1`、系统优先工具链和 `-j4` 仅重建受影响
integration target，`rc=0`，耗时 37.904s。结果目录
`.codex-tmp/spec187-segmented-input-r1/` 保留原始 stdout/stderr 与退出码：

| Selector | Cases | Result | Elapsed / max RSS |
| --- | ---: | --- | --- |
| `RequestScopedSelection/*` | 4 | `rc=0`, no errors | 20.74s / 188,928 KB |
| `RequestScopedResponseConfidentiality/*` | 4 | `rc=0`, no errors | 16.27s / 46,760 KB |

其中长输入 case 的 C++ 断言观察到 6,555,271 bytes → 1,601 signed Data
segments；每段包含 segment name 和统一 `FinalBlockId`，最大 wire 小于 8,800
bytes，Prefix 与后续精确 segment Interest 均被 relay，且 handler 检查在至少
全部 segments delivery 后才收到完整 payload。四用例整套首次运行曾出现一次旧
fixture 5s timeout，原始日志 `request-scoped-suite.log` 保留；单独重跑和第二次
整套均通过，因此该首次边界不作为功能 PASS。

这项证据证明当前 C++ User/Provider DummyFace 链的分段发布、SegmentFetcher
组装和小输入兼容性；它没有消除当前源码 MiniNDN r42 在 Controller 析构/证书
注册边界的 `corrupted size vs. prev_size`，也不把历史 r39 或本测试外推为当前
源码的 MiniNDN、SIF/APP 或 Tiger qualification。
