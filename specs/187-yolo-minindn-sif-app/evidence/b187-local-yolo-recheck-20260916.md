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

## 2026-09-16 current-source local MiniNDN confirmation

为确认“先本机 MiniNDN、后 SIF”的执行顺序仍适用于当前工作区，使用同一
candidate-bound package/config/input、`build-spec187-local-nac-r1` 和匹配的
NDN-SVS 运行库新建 root-owned state/output 目录重放 Y-A。r50 的第一次尝试在
MiniNDN 之前被 `STATE_ROOT_OWNER_MISMATCH` 拒绝；修正目录所有权后，第二次尝试
因最小化 sudo 环境缺少 `SHELL`，在 MiniNet `popen(shell=True)` 控制进程启动边界
得到 `KeyError`。两次均未进入协议请求链，原始日志分别为
`.codex-tmp/spec187-local-yolo-r50.log`，输出目录为
`results/spec187-local-yolo-r50/`。

r51 保留上述失败边界后使用完整的 `HOME/USER/LOGNAME/SHELL/TERM/LANG` 环境，
同一源码、二进制、candidate-bound 输入和新的 root-owned 目录完成 Y-A：退出码为
0，`SPEC180_CASE_RESULT status=PASS case=Y-A`，
`results/spec187-local-yolo-r51/subcase-result.json` 为
`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，native result 为 7,267
bytes。Controller、Repo、Provider、User 和 selector 均完成清理；Controller
日志观察到 ACK、response 和 Provider execution。r51 仍只是当前源码的 host
MiniNDN local PASS，不是 SIF/APP 或 Tiger qualification；SIF 构建继续暂停。

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

## 2026-09-16 C++/SVS dependency-aligned MiniNDN replay

此前 native receipt 的运行时探针暴露了真实依赖边界：框架头文件声明了
`subscribeToProducerWithCatchUp`，但 Waf 选择的 `/home/tianxing/NDN/ndn-svs/build`
旧二进制没有导出该符号；直接运行会在 Python import 阶段以 undefined symbol
失败，不能作为 MiniNDN 结果。该边界已保留在
`.codex-tmp/spec187-framework-svs-reconfigure-20260916-r1-build.log`。

使用相同源码 `9f2d8a4`，以本机 Boost 1.71/ndn-cxx 前缀重新构建
`/home/tianxing/NDN/ndn-svs/build-spec187-local`，导出目标符号；随后重新配置并
以系统工具链 `-j4` 重建 Spec187 受影响目标，Waf `205/205` 成功，耗时
`12m2.718s`。Python binding 也在同一 source/build pair 上重建，native identity
verify 返回 `SPEC180_NATIVE_IDENTITY_OK`。最终 manifest SHA-256 为
`2817afc19a61ba49c09f4720b184d4d01485c2b2ac5aa98a162dc6d0ad7215d7`，运行时实际
映射为：

| Library | Resolved path | SHA-256 |
| --- | --- | --- |
| `libndn-svs.so` | `/home/tianxing/NDN/ndn-svs/build-spec187-local/libndn-svs.so` | `c42ccaa97bf2b37de09a99244a5aed3c02106b2de19169362371f2c9aa3a374a` |
| `libndn-cxx.so.0.9.0` | `.local-boost171/lib/libndn-cxx.so.0.9.0` | `508f0fd9020947402f9b5c5b654ebe85541c097293ea3f625bfdbcbb7bc3a712` |
| `libnac-abe.so` | `nac-abe-integration-182/install-spec187-nac-r1/lib/libnac-abe.so` | `4c1fb004c6e516c8191c9e093cb86667cfd328ccc7585b905da00402ea7f24d1` |

在该依赖身份上以全新的 root-owned state/output 目录运行维护脚本
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A` 两次：

| Run | Exit | Native result | Subcase | Child cleanup |
| --- | ---: | --- | --- | --- |
| r48 | 0 | `7,267` bytes，`SPEC187_NATIVE_REQUEST_PASS` | `PASS / TERMINAL_RESPONSE / TERMINAL_RESPONSE_VERIFIED` | controller/authority/provider/user exit 0；Repo 为终止请求的 130 |
| r49 | 0 | `7,267` bytes，`SPEC187_NATIVE_REQUEST_PASS` | `PASS / TERMINAL_RESPONSE / TERMINAL_RESPONSE_VERIFIED` | controller/authority/provider/user exit 0；Repo 为终止请求的 130 |

两次使用同一 candidate-bound package manifest SHA-256
`9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d`，均观察到
Provider execution completed、终端响应及请求级 plan/model digest；运行后未留下
NFD、Controller、Provider 或 selector 进程。原始记录分别为
`.codex-tmp/spec187-local-yolo-r48.log`、`.codex-tmp/spec187-local-yolo-r49.log`
及 `results/spec187-local-yolo-r48/`、`results/spec187-local-yolo-r49/`。

该记录将“当前源码 + 依赖身份一致”的本机 MiniNDN Y-A 正向链从待重放状态推进
为两次可重复 host run；它仍不等于 SIF/APP host-gate、同 pair SIF execution、
MiniNDN negative path 或 Tiger qualification PASS。

## 2026-09-16 request-scoped segmented-input selector rerun

为直接复核分段输入用例，先运行
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput`。第一次命令
未把匹配的 NDN-SVS build 放在动态库搜索路径首位，`ldd` 显示实际加载了旧的
`/home/tianxing/NDN/ndn-svs/build/libndn-svs.so`；该运行在测试夹具的请求发布边界
失败，未得到协议结果，原始日志为
`.codex-tmp/spec187-segmented-input-r1/selector-rerun-20260916.log`；对应的动态库
解析记录为 `.codex-tmp/spec187-segmented-input-r1/ldd-stale-20260916.txt`。

修正 `LD_LIBRARY_PATH` 为先加载
`/home/tianxing/NDN/ndn-svs/build-spec187-local` 后，同一 selector 重新运行并返回
`rc=0`、`No errors detected`；日志为
`.codex-tmp/spec187-segmented-input-r1/selector-rerun-20260916-matched.log`，匹配
依赖解析记录为 `.codex-tmp/spec187-segmented-input-r1/ldd-matched-20260916.txt`。
这次结果与既有四用例整套证据一致：分段传输链通过，旧依赖选择失败不计为产品失败，
也不扩大为 MiniNDN/SIF/Tiger qualification。

本轮组合审查使用不可变快照 `.codex-tmp/review-spec187-local-yolo-r48-r49-20260916/`；
`sha256sum -c sha256sums.txt` 的 5 个文件全部通过，官方 `review-agent` 返回
`STATIC_PASS`，无 P0/P1/P2/P3。审查覆盖 static/contract、compile-link evidence、
runtime/lifecycle、compatibility 和 tests/evidence/docs 五个 lane，并确认
T001/T002/T003 仍为 `PARTIAL`、T004 为 `WAITING_EXTERNAL_INPUT`；SIF/APP host gate、
same-pair SIF execution、negative path、pre-pack consumer 与 Tiger/Slurm 仍未观测。

## 2026-09-16 current-source local YOLO replay after segmented-input changes

本轮严格按“先本机 MiniNDN、后 SIF”执行。现有构建收据先以
`SPEC180_NATIVE_IDENTITY_REJECTED: STALE_SOURCES` 拒绝；重新配置时首次缺少锁定的
Rust cargo，随后使用已封存的 Rust 1.90/cargo-home 完成配置。一次长编译在外部会话
终止信号下停在 124/205，残留的零字节 `OnnxRuntimeModelRunner.cpp.7.o` 被识别并
删除；重链后受影响 Native/DI 目标 Waf `205/205` 成功，耗时 3m01.929s。最终收据
`SPEC180_NATIVE_IDENTITY_OK`，绑定扩展也在相同 NDN-SVS/NAC-ABE/Boost 依赖身份上
完成重建。原始记录为 `.codex-tmp/spec187-local-yolo-current-build-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-configure-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-configure-r2-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-native-build-r3-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-native-build-r4-20260916.log` 和
`.codex-tmp/spec187-local-yolo-current-waf-r5-20260916.log`。

官方 `review-agent` 对 exact-SIF 输出根修复先发现并闭合了两个 P2：输出根必须位于
`ROOT/results` 的严格子目录，拒绝 symlink、`..` 越界和规范化后逃逸；host MiniNDN
临时目录行为保持兼容，复审结果为 `STATIC_PASS`。Python 定向检查为 9 passed。
两次已有目录重放分别在输出门禁处拒绝（r52: `OUTPUT_ROOT_NOT_EMPTY`，r53: 同一
残留目录），未启动 MiniNDN，原始记录为 `.codex-tmp/spec187-local-yolo-r52.log`
和 `.codex-tmp/spec187-local-yolo-r53.log`。

使用纳秒时间戳生成的新 run-id
`spec187-local-yolo-1789592534950657908`，同一 candidate-bound config/input、
当前源码、当前 native receipt、完整 root 环境和新的 root-owned state/output 根
完成 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A`：退出码 `0`，
`results/spec187-local-yolo-1789592534950657908/subcase-result.json` 为
`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`。C++ selector/生产日志观察到
`ACK_CLOSED`、`SELECTION_COMMITTED`、`SELECTION_ACCEPTED`、
`PROVIDER_EXECUTION_COMPLETED` 和 `SPEC187_NATIVE_REQUEST_PASS`；6,555,271-byte
request-scoped input 以 4,096-byte chunk 发布为 1,601 个 Data segments，终端响应
为 7,267 bytes。Controller、authority、Provider、User 和 selector 均完成清理，
子进程 exit status 为 0（Repo 终止请求为 130）。

这次是当前源码的本机 host MiniNDN YOLO Y-A 正向 PASS；没有启动 SIF/Apptainer、
TigerCluster 或 negative path，因此不改变 T001/T003 的 `PARTIAL`、T004 的
`WAITING_EXTERNAL_INPUT`，也不把它外推为容器或集群资格。

## 2026-09-16 corrected-dependency local YOLO replay

为排除 r42 使用旧 NDN-SVS 环境造成的误判，使用同一 candidate-bound package、
config、input、native selector 和 envelope key，只替换为已验证的
`ndn-svs/build-spec187-local` 及对应 Boost/NAC-ABE 搜索路径，并隔离新的
root-owned state/output 根。运行 ID 为
`spec187-local-yolo-1789593707473878369`，原始环境和输出分别保留在
`.codex-tmp/spec187-local-yolo-1789593707473878369.env`、
`.codex-tmp/spec187-local-yolo-1789593707473878369.log` 和
`results/spec187-local-yolo-1789593707473878369/`。

`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A` 返回 `rc=0`，
`subcase-result.json` 为 `PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，
终端结果含 `SPEC187_NATIVE_REQUEST_PASS`。User 日志观察到
`NDNSF_DI_NATIVE_ACK_CLOSED`、`NDNSF_DI_NATIVE_SELECTION_COMMITTED` 和
1,601 个 4,096-byte request-scoped input segments；Provider 日志观察到
`NDNSF_DI_NATIVE_SELECTION_ACCEPTED` 与
`NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED`，终端结果为 7,267 bytes。
Controller、Authority、Provider、User 和 selector 均完成退出；Repo 的终止请求
exit 130 与既有 Y-A 行为一致。该重复运行进一步确认当前本机 host MiniNDN
正向链可运行；仍未启动 SIF/APP、negative path 或 Tiger，因此不改变
T001/T003 的 `PARTIAL` 或 T004 的 `WAITING_EXTERNAL_INPUT`。

## 2026-09-16 repeated current-source local YOLO replay

为确认本机优先顺序下的可重复性，继续复用同一 candidate-bound package、manifest、
config、input、native selector、envelope key 及匹配的
`ndn-svs/build-spec187-local`。第一次重试先由普通用户创建新的 state/output 根，
维护脚本在 MiniNDN 启动前以 `STATE_ROOT_OWNER_MISMATCH` 返回 78；原始记录为
`.codex-tmp/spec187-local-yolo-1789595123000000000.log`，没有协议副作用。

将同一两个目录改为 root-owned 后，以新的 run ID
`spec187-local-yolo-1789595123000000000` 重跑
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A`：退出码为 `0`，
`subcase-result.json` 为 `PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，
native result 为 7,267 bytes。User/Provider 日志再次观察到
`NDNSF_DI_NATIVE_ACK_CLOSED`、`NDNSF_DI_NATIVE_SELECTION_COMMITTED`、
`NDNSF_DI_NATIVE_SELECTION_ACCEPTED`、
`NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED` 和
`SPEC187_NATIVE_REQUEST_PASS`；1,601 个 4,096-byte 输入分段保持不变，
Controller、Authority、Provider、User 和 selector 清理完成，Repo 的终止请求为
既有的 exit 130。

这次重复运行进一步支持“先本机 MiniNDN、后 SIF”的顺序，但仍只证明 host
MiniNDN 正向链。SIF/APP host-gate、同 pair SIF execution、negative path 和
Tiger qualification 未执行，T001/T003 继续 `PARTIAL`，T004 继续
`WAITING_EXTERNAL_INPUT`。

## 2026-09-16 current C++ segmented-input selector recheck

在当前工作区和既有 `build-spec187-local-nac-r1` 二进制上单独复跑
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput`，退出码为
`0`，1 个测试用例、6,449 个 assertions 全部通过。该 selector 仍覆盖
6,555,271-byte 输入、4,096-byte 加密分段、最终 Data wire 不超过 8,800 字节、
Provider `SegmentFetcher` 重组以及分段 AEAD 绑定；原始输出为
`.codex-tmp/spec187-segmented-input-focused-current-20260916.log`。

本次没有修改生产源码，也没有启动 Qwen、SIF 或 Tiger；它只重新确认当前源码的
分段发布/消费边界，不改变 T001/T003 的 `PARTIAL` 状态。

## 2026-09-17 large-fetch ownership regression

为降低真实 Qwen 大对象路径的复制放大，`ServiceProvider` 的
`fetchAndDecryptLargeDataUntil` 现在直接共享 `SegmentFetcher` 的只读
`ConstBufferPtr`，并在解码后通过 shared envelope 贯穿异步密钥和解密回调。
官方 review-agent 对冻结快照返回 `STATIC_PASS`，无 P0/P1/P2；五个 lane
中的 runtime RSS、延迟取消和恶意延迟回调仍未观测。

当前依赖身份下的受影响构建为 Waf `233/233`、`1m54.92s`、峰值 RSS
`2,868,132 KB`、无 swap。设置 `NDNSF_SPEC182_BIN_DIR` 和匹配的 NAC-ABE
预加载后，`Spec175NativeAssembly/*` `7/7`、
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput`
`1/1` 均 `rc=0`。首次 selector 因未提供 worker 目录在 fixture preflight
停止，随后已按真实边界修正；所有原始日志保留在 `.codex-tmp/`。

本轮清理了已结束且可重建的 `/tmp` 构建/审查沙箱，释放约 `28 GiB`；
`.codex-tmp/qwen06b-local-20260916`、当前 build、`results/`、模型和原始
失败证据均保留。当前根分区约 `43 GiB` 可用。该回归没有启动真实 Qwen、SIF
或 Tiger，不改变 T001/T003/T007 的 `PARTIAL` 状态。

2026-09-17 Qwen r12/r13 local boundaries：r12 将 `LD_PRELOAD` 传给 `ldd -r`，在
MiniNDN 前递归停止；r13 去掉 preload 后完成 MiniNDN 角色启动，但约 119 秒时
`MemAvailable` 降至 2 GiB 安全线以下，受控停止，未观察 terminal request 或 numerical
oracle。r13 仍为 `RESOURCE_BOUNDARY`，不改变本 Spec 的 YOLO host PASS 或 SIF/Tiger
状态。详见 [Qwen replay evidence](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--r12r13-launch-and-resource-boundaries)。

2026-09-17 Qwen r11 root preflight：首次 root launch 遗漏维护的 `PYTHONPATH`，在
MiniNDN 启动前返回 `MODEL_ONNX_VALIDATOR_UNAVAILABLE`。修正后的 import 已在两种身份
确认，但 r11 不计作 Qwen、SIF 或 Tiger 结果；r10 `RESOURCE_BOUNDARY` 仍开放。详见
[Qwen replay evidence](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--r11-root-preflight-boundary)。

2026-09-17 Qwen r14 monitored replay：WireEncode 共享 buffer 修复后的新 run 在正确
环境下完成 Controller、Authority、三个 Provider 和 requester 启动，Provider 均发出
签名 V3 offer；约 117 秒时进程树 RSS `6,737,232 KB`、`MemAvailable=1,651,808 KB`
触发安全监控并返回 `rc=137`。未观察 Selection、Provider execution、terminal response
或 numerical oracle；r14 是 `RESOURCE_BOUNDARY`，不改变本 Spec 已有的 YOLO host PASS，
也不改变 SIF/Tiger 状态。所有 r14 子进程已核验退出，磁盘约 43 GiB 可用。详见
[Qwen replay evidence](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--r14-monitored-replay-after-shared-wireencode-repair)。
下一步应先实现有界或 file-backed 的分段发布并用 C++ 资源探针验收，再重跑完整 Qwen；
不能提高阈值或删除模型/构建/证据来绕过该边界。
