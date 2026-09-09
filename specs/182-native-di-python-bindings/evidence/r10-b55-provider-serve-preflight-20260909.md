# R10-B55 Provider Serve Preflight 2026-09-09

## Batch allocation

本批只验证 repaired standalone Provider 的真实 `--serve` 启动、Face/ServiceProvider 创建、
NativeInferenceProvider host 注册以及 readiness 失败边界。共同入口是
`DI_NativeProviderExecutable.cpp::main --serve`；使用从四角色 plan 派生的 metadata-only
manifest，避免把 R10-B51 check-only 的预装 artifact 路径带入 serving。稳定观察出口是
`NDNSF_DI_NATIVE_PROVIDER_SERVE_READY`，不是 terminal request/result 或 T016 qualification。

## First failure boundary and changed gate

第一次探针漏传 `--serve`，`parseArgs` 以 exit `2` 返回
`exactly one of --check-only or --serve is required`；raw output 保留在
`.codex-tmp/spec182-r10-b55-provider-serve-preflight-20260909/serve.log`，没有创建 Face。
这是 harness 参数构造边界，不是 serving 或协议结果。

修正后显式加入 `--serve`，继续使用 metadata-only manifest 和 `timeout --kill-after=2s 12s`。
进程成功创建 Face、KeyChain、ServiceProvider、NativeInferenceProvider host，并打印
`NDNSF_DI_NATIVE_PROVIDER_PLAN_READY`、`NDNSF_DI_EXECUTION_LEASE_SERVICE_READY` 和
`NDNSF_DI_NATIVE_PROVIDER_SERVE_READY`。随后后台 permission/readiness 轮询因当前
Controller 未安装 `/Inference/NativeTracer` Provider permission，打印
`NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED`；事件循环直到 timeout，返回 `124`。该首个
运行边界在 `serve-retry.log` 保留，不能解释为请求失败或资格失败。

## Coverage matrix

| Lane | Covered files/symbols and check | Result |
| --- | --- | --- |
| production entry/callers | `DI_NativeProviderExecutable.cpp::main`/`parseArgs` → Face/ServiceProvider → `NativeInferenceProvider::serve` → readiness loop | covered |
| implementation/wire | metadata-only manifest, DATA_DRIVEN_V2 serve preconditions, provider host registration, lease service and readiness markers | covered |
| test/harness/oracle | generated metadata-only manifest, bounded timeout probe, explicit startup/serve/provision markers and first-boundary classification | covered |
| build/source closure | repaired R10-B50 `di-native-provider` SHA `4be6b29...`; `ldd` reports no `not found`; no rebuild in this batch | covered |
| migration/evidence | first missing-mode CLI boundary and corrected serve log/result retained; no Python, requester or terminal response | covered |

## Review trace

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）执行只读审查；基线为
source checkpoint `6876e616`，范围覆盖 `parseArgs` mode gate、serve preconditions、
metadata-only manifest shape、Face/KeyChain/ServiceProvider construction、host registration,
detached install task, permission/readiness loop, timeout harness and marker interpretation。
首个参数边界被列为 harness miss；修正命令后的静态复审没有新的 P1/P2/P3。

## Verification

```text
# first probe (missing --serve)
timeout --kill-after=2s 12s env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  di-native-provider --plan <plan> --manifest metadata-only-manifest.json ...
-> exit 2; exactly one of --check-only or --serve is required

# corrected bounded serve probe
timeout --kill-after=2s 12s env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  di-native-provider --plan <plan> --manifest metadata-only-manifest.json \
  --service /Inference/NativeTracer --provider /example/native-provider \
  --group /NDNSF-DistributeInference/example/group \
  --controller /NDNSF-DistributeInference/example/controller \
  --serve --no-serve-certificates --disable-tokens --tracer-deterministic-runner \
  --permission-wait-ms 2000 --offer-backend onnxruntime-cpu
-> exit 124 (timeout); Face/ServiceProvider/host/serve markers present; permission not installed
   and provisioning failed; no terminal request/result
```

The Provider binary SHA-256 is
`4be6b29acb10b29792757a23ce0ffc4f098f39e9cec2f0f47f4175d03bdae75a`; `ldd` reports no missing
libraries. Raw manifest, command output and exit records are under
`.codex-tmp/spec182-r10-b55-provider-serve-preflight-20260909/`.

## Batch retrospective

- `static`: `parseArgs` and serving preconditions were read before execution; no P1/P2/P3 finding
  remained after the explicit-mode review.
- `compile/link`: none; the repaired Provider binary was reused and its SHA/`ldd` closure checked.
- `runtime/test`: the first probe only exposed a missing CLI mode; the corrected probe reached
  `SERVE_READY` and then exposed the real Controller permission boundary (`rc=124` after bounded
  timeout, with `PROVISION_FAILED` recorded). No protocol request was attempted.
- `unobserved`: authenticated Selection, post-Selection worker/model assembly, requester/Provider
  transport, terminal Response, maintained caller/no-Python and T016 qualification remain open.

The batch stopped at its declared serve/readiness exit and did not absorb requester transport or
qualification members. The timeout is a bounded runtime observation, not an efficiency measurement.

## Closure decision

`CLOSED_FOR_VALIDATION` for Provider Face/ServiceProvider/NativeInferenceProvider serve registration
and explicit permission/readiness failure classification under the repaired binary.
`OPEN_FOR_NEXT_BATCH` for Controller permission installation, authenticated Selection, post-Selection
assembly, independent requester/Provider transport, terminal Response and T016 qualification.
This focused result does not close T009-C, T010-B or any parent task.
