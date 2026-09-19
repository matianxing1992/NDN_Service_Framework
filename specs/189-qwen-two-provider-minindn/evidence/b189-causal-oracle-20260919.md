# T007 Causal Provider Oracle

## Design binding

2026-09-19 01:28 -05:00。T007 items 2/3 与 contracts/placement.md；HEAD `f34fa525`
加既存工作区。目标是完整 CLI 复用材料门与 Provider 因果门，首段无需上游 fetch，
后段必须完成 NDN dependency 事件，assembly 与 dependency 允许交错，末段有 terminal。
28 层只作为 Spec189 Qwen3-0.6B 候选判据，不新增全局模型机制。

`EXECUTION_ENTERED` 是调度准入，不是模型 compute start；仅凭该日志不能证明输入在
开始计算时已就绪。输出正确性、缓存/多请求复用及真实网络仍需 T007/T009 的对应证据。
Risk class：validation correctness；Dynamic profile：none（同步离线日志检查）。

## Review trace

冻结 r1：`.codex-tmp/spec189-causal-oracle-review-r1/`，含新增 header/test，保存本单元
开始前 CLI 副本；由 `/root/spec189_review2` 使用官方只读 review-agent 审查。
官方技能 SHA256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。

r1 NOT_STATIC_PASS：reviewer 提出 ASSEMBLY 与 ENTERED 顺序问题，主会话要求沿
lambda 实际调用链复核（不能由 lambda 定义的行号推断执行时间）。同时发现正常 CLI
未执行共用 placement/material 门，terminal 未绑定尾段；还需覆盖多 token 下重复的
runner preparation/ready 周期。未通过静态门，不启动构建。

## r2 repair design

reviewer 沿实际调用撤回 r1 的 ENTERED/ASSEMBLY 顺序发现：2330 是 lambda 定义，
实际在 ENTERED 后由 ProviderRoleWorker 调用。确认新的多 epoch 问题：每个 epoch
会调用准备 lambda，不能要求 ASSEMBLY_STARTED/RUNNER_READY 全请求唯一。

r2 在 NativeProviderHandler 的每个 request/role 准备回调 owner 下持有独立 atomic
sequence；每次调用分配 `preparationId`，ASSEMBLY_STARTED 与 RUNNER_READY 带同一 ID。
该 ID 只是 runner preparation 调用身份，不冒充 inference epoch，不改变 operation
status sequence、公开 API、授权或 wire。checker 按 ID 成对检查，其他核心阶段仍唯一。
缺失/复用 ID、半完成或错序必须拒绝。此内部可观测性字段不改变当前/目标公开 API。

正常 CLI 先执行与 placement-only 相同的 placement/material prerequisites，然后
验证 Provider stages；已验证 layerEnd=28 的末段必须 terminal，前段不能冒充终态。
新增 standalone C++ CLI fixture 运行真实 oracle executable，覆盖正例、漏材料、漏
尾段终态、前段伪终态与范围缺口；这些都是判据反例，不是网络/模型生产验收。

冻结 r2：`.codex-tmp/spec189-causal-oracle-review-r2/`，包含生产 emitter、两个共享
header、CLI、两个新增 C++ fixtures 与 Waf 注册；r1/base 保存修改前 caller/emitter。
同一 reviewer 的任务和组合复审均为 STATIC_PASS（官方技能身份同上）。

| Lane | Static coverage |
| --- | --- |
| production entry/callers | covered：NativeProviderHandler emitter → ProviderRoleWorker / NativeEpochCoordinator 实际调用 |
| implementation and wire | covered：per-invocation shared atomic、同 ID 配对、header inline 与身份字段；无 wire 变化 |
| test/harness/oracle | covered：Provider 多周期正反例；真实 CLI subprocess 对材料/范围/尾段门反例 |
| build/source closure | covered static：两个 fixture 独立 target、CLI 链接现有 DI、生产 handler 既有源闭包；待编译验证 |
| migration/evidence | N/A API migration；旧缺 preparationId 日志不能证明新门；真实网络资格仍 PARTIAL |

| Frozen file | SHA256 |
| --- | --- |
| NativeProviderHandler.cpp | `24c73da440e3e042043a8a7cd6ef2a08d0c65822356f59829263267f287bb7c2` |
| Spec189MaterialFetchOracle.hpp | `e9d478e0c177a7d5105ef55728abfed76c2b30d5d977f1b16c05094907c29caf` |
| Spec189ProviderStageOracle.hpp | `e5b3f208d3f53e067c02c6e7d021e1ea2e5be485ec19241ac3ea8ad952d93dac` |
| Spec189TwoProviderOracle.cpp | `a4244ac4cb331604250384350ef20747a925637eb1577a7d0a48719c3cdbbc9c` |
| examples/wscript | `692b4ea831435a38dc1da46446a4f2bbbf47f11ab83cef8bef4419e86c5db0a3` |
| provider-stage-oracle.cpp | `280b0e7be622bf377eeb5dfc209a85a88d354b2d4ff593221bb6f5e6593d5d6d` |
| cli-oracle.cpp | `84d2f463c6807abefb4935bd6f0e0f45847480c1380ad325bef3bf265b070101` |

审查保留限制：DEPENDENCY_FETCH 未记录 edge endpoint/operation 身份，因此这些 stage
只能证明该 role 有 fetch 完成，不能证明特定已选输入的值与身份；必须用生产 endpoint
与独立输出验收补足，不能提升为网络或模型 PASS。

## Validation

复用 `build-spec189-b189-3-global-r3`，使用 `/usr/bin/python3 ../waf build -j4`
选择 `ndnsf-distributed-inference,spec185-provider-assembly,spec189-two-provider-oracle,
spec189-provider-stage-oracle-tests,spec189-cli-oracle-tests,spec189-material-oracle-tests`。
统一增量构建 PASS：2m54.872s。Provider 专用目标较旧，本次更新其受影响 DI 对象；
未重编外部依赖、Repo 或 UAV。期间无 swap，可用内存采样最低约 3.5 GB。

| C++ check | Result |
| --- | --- |
| spec189-material-oracle-tests | 15 cases PASS |
| spec189-provider-stage-oracle-tests | 18 cases PASS |
| spec189-cli-oracle-tests <absolute oracle binary> | 5 cases PASS |
| Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse | PASS，约 4.1s |

Provider selector 使用 run-owned `NDNSF_REQUEST_LARGE_DATA_DIR` 与当前 build root
`NDNSF_SPEC182_BIN_DIR`。首次默认日志未显示机器证据，再以同一二进制、同一 selector
设置 `NDN_LOG=ndnsf.di.RuntimeEvidence=WARN` 采集日志，通过并观察到真实 handler 的
ASSEMBLY_STARTED/RUNNER_READY 均携带 preparationId=1。该 fixture 经 Provider::serve
和 Core Selection/Response，但注入 deterministic runner，因此不是 Qwen 模型资格。

原始日志 `.codex-tmp/spec189-causal-oracle-build-r1/`：`build.log/build.rc`、
`material.log/material.rc`、`stages.log/stages.rc`、`cli.log/cli.rc`、
`provider.log/provider.rc`、`provider-evidence.log/provider-evidence.rc`。
七个源码/注册输入逐字节匹配 r2 审查快照；独立核对通过。

随后使用 `scripts/install-global-target.sh --build-dir build-spec189-b189-3-global-r3
--target ndnsf-distributed-inference --jobs 4` 完成全局同步；`install.rc=0`，Waf install
4.491s。DI build/global SHA256 一致；requester 的 ldd 解析 DI/Core 到 `/usr/local/lib`、
ORT 到 `/opt/onnxruntime/lib`。oracle 只使用 header canonical JSON，实际 ldd 无 DI/Core
依赖；Waf use 声明不能被当成生产库动态链接证明。本批无公共 ABI 变化，不重建 Python binding。

| Tested artifact | SHA256 |
| --- | --- |
| DI build/global | `353dda9b3ccf5f7869ac49630054be7dd19c4541c3f8b2920943deb12e518137` |
| spec185-provider-assembly | `4692c369e9bceac149e2d4f56b705395dbf64d17b0a0bdf356ac5683389f3af9` |
| spec189-two-provider-oracle | `c706407dd284a02c2af26b013cb8e1c43f1720d9def1b2f5d4be6083d0ecb6c0` |
| spec189-provider-stage-oracle-tests | `5ee194f1ec1f268b7480883d1a5410cd1611004ed04b470a66fb6ccd27b2dcd5` |
| spec189-cli-oracle-tests | `0d1e3781071ac1fbac4968d2f9b275045a5b44b804876cd8e3aaf7c9e9482451` |

## Closure

本批事件判据与组合 CLI 门已通过 C++ 检查；T007 仍 PARTIAL，独立输出值、特定
upstream endpoint/compute-start 因果、真实多 token 运行、同 handle 复用尚未验收。
无真实 Qwen 两 Provider MiniNDN PASS。CLI 的既存相关接线与本批合并归档，其他
requester、runner 配置及无关脏文件保持独立。

Retrospective：static 发现多 epoch unique-stage 误拒、正常 CLI 漏材料门和尾段身份；
最初按 lambda 定义行推断调用顺序的发现已撤回，依据实际调用链修正。compile-link 与
runtime-test 本批无失败；unobserved 见上。摘要辅助命令曾因 Python 无 file_digest
API 失败，改用 sha256sum；不属于产品失败，不使测试结果失效。

## B189-3/T007 rerun after checkpoint review — 2026-09-19

The bounded emitter/oracle range was re-frozen at base `b8396d14` and received
official read-only `STATIC_PASS` from `/root/spec189_review2`. The review snapshot
and identities are retained under `.codex-tmp/spec189-b189-3-oracle-review-r1/`;
the known diff SHA is `c041e89f3ea77a2ae076cd4f0ad09528f8aa356c2c88a7f6cc076305f298c617`.
It covered `NativeProviderHandler.cpp`, the two shared oracle headers, the CLI,
the two new fixtures and `examples/wscript`; no public API or wire field was
added. The review explicitly leaves endpoint payload causality, compute-start
readiness, numerical output, reuse, real Qwen and MiniNDN unobserved.

The first retry command was an invocation error (`python3 waf` from the build
directory); it exited before Waf with code 2. Its raw record is
`.codex-tmp/spec189-b189-3-oracle-build-r2/build.log` and `build.rc`, and the
boundary is indexed in `docs/failure-log.md`. The canonical retry used:

```text
build-spec189-b189-3-global-r3: /usr/bin/python3 ../waf build \
  --targets=ndnsf-distributed-inference,spec185-provider-assembly,\
spec189-two-provider-oracle,spec189-provider-stage-oracle-tests,\
spec189-cli-oracle-tests,spec189-material-oracle-tests -j4
```

It completed successfully in `16.228s` (`build.rc=0`). From the repository
root, the same build candidates then passed:

| C++ check | Result |
| --- | --- |
| `spec189-material-oracle-tests` | 15/15 PASS |
| `spec189-provider-stage-oracle-tests` | 18/18 PASS |
| `spec189-cli-oracle-tests` with the absolute oracle binary | 5/5 PASS |
| `Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/AuthenticatedSelectionRunsProviderAssemblyRunnerAndResponse` | PASS, `NDN_LOG=ndnsf.di.RuntimeEvidence=WARN` |

Run logs are in `.codex-tmp/spec189-b189-3-oracle-tests-r2/`. The tested
binary hashes are:

| Artifact | SHA256 |
| --- | --- |
| `spec189-material-oracle-tests` | `8ebf49706ddd4003662f1c3da02bca1a0cb58a7220040ffad874d15d9a1b2949` |
| `spec189-provider-stage-oracle-tests` | `4306b0822243cb0bfd148f6a80eb786e8157c801cf959b8f6d429e1be58f277f` |
| `spec189-cli-oracle-tests` | `6e918787b6043631fc56012971dda436dd82a9e77806d3421c8a212f768b317b` |
| `spec189-two-provider-oracle` | `bd7e776b19d2cc2ac8d79c130f0780a650f9d325bbc045580beef88095306f3e` |
| `spec185-provider-assembly` | `2b8b3fe66af3df13fb4e5ebd287a37c0ca41afd8b412a8f77350a98c707747d2` |

The selector resolved system Boost 1.71 from `/lib/x86_64-linux-gnu`,
NDN-CXX/NAC-ABE from `/usr/local`, and ONNX Runtime from `/opt/onnxruntime`;
no `not found` dependency was reported. This is a compile/link and focused
runtime checkpoint only. T007 and the overall Spec remain `PARTIAL`.
