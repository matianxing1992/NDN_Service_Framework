# R10-B38 T016 Runtime Context Recheck

## Scope

R10-B37 后重新执行 T016-A 的启动前 preflight，使用新的 raw run 目录，确认主机运行状态
变化是否已经提供 MiniNDN qualification 所需的 owner context。本批不改 manifest、业务
进程、网络拓扑或产品源码；只记录真实首拒绝边界。

## Observed first boundaries

- 系统层面现在存在 `/run/nfd/nfd.sock`，且 `nfd` 进程运行；但没有 MiniNDN application
  node 的 PID/starttime、network namespace、独立 socket 和 peer metadata。
- 默认 registration-only campaign 使用 fresh `.codex-tmp/spec182-t016-r6/`，在启动业务
  前返回 exit 2、`status=UNQUALIFIED`、`reason=MININDN_NODE_CONTEXT_NOT_PROVIDED`。
- 显式 `--execute-owner` 使用 fresh `.codex-tmp/spec182-t016-r7/`，当前 UID 1000 在创建
  MiniNDN owner topology 前返回 exit 2、`status=UNQUALIFIED`、`reason=MININDN_REQUIRES_ROOT`。

这两次运行都没有启动 requester/provider 业务进程、namespace、网络请求或 C++ 构建；系统
NFD socket 的存在不能替代由 root MiniNDN owner 导出的 node context。

## Review trace

官方只读 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）按 preflight 范围
复核：campaign owner 的两种入口、fresh output 目录、manifest registration、状态/退出码
和“未启动业务”的边界。无 P1/P2/P3 产品 finding；缺少 root MiniNDN owner context 是外部
环境前置，不是协议结果。

## Verification

```text
if [ -S /run/nfd/nfd.sock ]; then echo NFD_SOCKET_PRESENT; fi
pgrep -a nfd

python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-t016-r6/manifest.json \
  --output .codex-tmp/spec182-t016-r6/result
# exit 2; result/result.json: UNQUALIFIED / MININDN_NODE_CONTEXT_NOT_PROVIDED

python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-t016-r7/manifest.json \
  --output .codex-tmp/spec182-t016-r7/result --execute-owner
# exit 2; result/result.json: UNQUALIFIED / MININDN_REQUIRES_ROOT

python3 specs/182-native-di-python-bindings/checklists/validate_design.py
git diff --check
```

`validate_design.py` 返回 `ok=true`（17 个父任务，当前父任务完成数仍为 3），没有把本批
preflight 误计为产品完成。

## Minimum Review Record

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `run_campaign`; `_run_owned_campaign`; T016-A manifest entry | Code/read of `Experiments/NDNSF_DI_NativeClosure_Minindn.py`; fresh command outputs | default and owner entry points stop before business execution |
| `implementation and wire` | `covered` | manifest `qualification` registration; node-context requirement; root guard | manifest parse and campaign owner source inspection | NFD socket alone does not satisfy node identity/socket/peer contract |
| `test/harness/oracle` | `covered` | T016 I01 registration; output `result.json` status/reason | fresh `r6` and `r7` result inspection | both are preflight statuses, not I01 protocol PASS/FAIL |
| `build/source closure` | `N/A` | no product source or build target changed | changed-path review; no build command | no native build applicable |
| `migration/evidence` | `covered` | tasks/plan, R6-B4 evidence, new r6/r7 raw outputs, failure log | link/status/diff checks | first boundaries preserved; T016 remains `PARTIAL/UNQUALIFIED` |

## Batch result

- **Review trace**: official review-agent path/SHA and both fresh run directories are recorded;
  no actionable product finding.
- **Closure decision**: `CLOSED_FOR_VALIDATION` for the context recheck; `OPEN_FOR_NEXT_BATCH` for
  qualification. No T016 or parent task is complete.
- **Static findings**: none; the owner explicitly requires root and identity-bound MiniNDN context.
- **Compile/build misses**: none; no product target changed.
- **Runtime/test misses**: expected preflight stop at both entry points; no protocol result or
  business marker was produced.
- **Build measurement**: `BUILD_NOT_APPLICABLE`.
- **Behavior result**: `STATIC_PASS`; `UNQUALIFIED` preflight; not `QUALIFICATION_PASS`.
- **Evidence / remaining**: `.codex-tmp/spec182-t016-r6/` and `r7/` remain immutable. A later
  attempt needs root MiniNDN owner to export requester/provider namespace, PID starttime, NFD
  sockets and peer metadata, then run the complete I01–I08/PO matrix in a fresh directory.

### Batch growth decision

The batch had one stable exit: determine whether current host state permits T016 owner startup.
No business process, selector, transport, or qualification case was added. Any such execution
starts a new Batch ID.

### Batch Retrospective

- `static`: no miss; owner and default preflight contracts were checked separately.
- `compile/link`: not applicable.
- `runtime/test`: both commands correctly stopped before protocol execution with explicit reasons.
- `unobserved`: root MiniNDN topology, node/netns metadata, cross-process transport, maintained
  callers/no-Python and all I01–I08/PO qualification cases remain open.
