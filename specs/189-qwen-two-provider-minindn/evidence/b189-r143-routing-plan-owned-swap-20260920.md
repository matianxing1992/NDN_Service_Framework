# B189 r143 Static-Routing Plan and Owned-Swap Boundary

**Date**: 2026-09-20
**Status**: `PARTIAL`
**Run**: `two-provider-global-r143`

## Scope

本记录同时固定两件事：r143 的真实运行边界，以及本轮对 MiniNDN Python
入口的限定修正。r143 启动时仍使用旧的 `Nlsr + NdnRoutingHelper` 组合；本轮
随后对入口进行静态修正，尚未用修正后的脚本重新运行模型。

## MiniNDN Python review

本机 MiniNDN upstream examples 将路由方式分开：

- `examples/nlsr/pingall.py` 使用 `AppManager(..., Nfd)` 和
  `AppManager(..., Nlsr)`；
- `examples/static_routing_experiment.py` 使用 `AppManager(..., Nfd)` 和
  `NdnRoutingHelper`。

Spec189 需要把 Controller、Authority、Provider、Requester 和 group 前缀
绑定到确定的应用前缀路由。入口因此改为 `Nfd + NdnRoutingHelper` 的静态路由
模式，不再同时启动 NLSR，避免重复 UDP face 和 FIB owner。`--nlsr-wait-s`
保留为 `--routing-wait-s` 的兼容别名，避免历史 launch 命令失效。

入口现在生成并校验 `minindn-node-app-plan.json`：

| Node | Apps |
| --- | --- |
| `memphis` | `Nfd`, `App_ServiceController`, `DI_NativeArtifactAuthority` |
| `ucla` | `Nfd`, `di-native-provider[provider-0]` |
| `arizona` | `Nfd`, `di-native-provider[provider-1]` |
| `neu` | `Nfd`, `DI_NativeRequester` |
| `wustl` | `Nfd` only (forwarder) |

该 Python 入口仍只负责拓扑、身份/文件、MiniNDN 系统 APP、C++ 子进程和
资源生命周期；请求规划、授权、Selection、组装、ORT 执行和 terminal 仍由
生产 C++ 程序负责。

## r143 runtime boundary

原始证据：

- launch log: `.codex-tmp/spec189-r143-launch.log`
- run root: `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r143/`
- supervisor: `runs/two-provider-global-r143/supervisor.json`
- resource samples: `runs/two-provider-global-r143/resource-samples.jsonl`

已观察：

- MiniNDN/NFD/routing 启动完成；
- 两 Provider 都达到 `NDNSF_DI_NATIVE_PROVIDER_READY`；
- 两 Provider 都产生 signed `ACK_DECISION`；
- 两 Provider 都产生 `NDNSF_DI_GRANT_VERIFICATION`，边界为
  `BEFORE_ASSEMBLY`。

首个停止边界：

- `RESOURCE_BOUNDARY:ownedSwap`；
- `ownedSwapBytes=268681216`，限制为 `268435456`；
- supervisor `cleanup=PASS`，无残留子进程；
- requester 为 shutdown consequence 的 `CANCELLED`。

未观察：`ACK_CLOSED`、`Selection`、`EXECUTION_ENTERED`、
`DEPENDENCY_FETCH`、`RUNNER_READY`、`EXECUTION_COMPLETED`、terminal
response、数值 oracle、repeat/drain 和资格 PASS。

## Five-lane disposition

- `static`: MiniNDN upstream routing examples and current Python entry were
  reviewed; the duplicate routing-owner configuration was corrected.
- `compile/link`: not applicable to this Python-only entry change; no C++
  rebuild was performed.
- `runtime/test`: `py_compile`, `--help`, node-app-plan helper, and
  `git diff --check` passed. After the checkpoint, a five-node MiniNDN smoke
  using `AI_Lab.conf`, `Nfd`, and `NdnRoutingHelper` published
  `/spec189/smoke` from `memphis` and observed it in `neu`'s FIB; cleanup
  passed. The real r143 run reached READY/ACK/grant but stopped at the host
  owned-swap boundary.
- `unobserved`: the corrected static-routing candidate has not yet completed a
  fresh MiniNDN run, and the full two-Provider execution/terminal/oracle path
  remains open.

No Spec189 task checkbox changes. T003, T005, T006, T007, and T009 remain
`PARTIAL`.
