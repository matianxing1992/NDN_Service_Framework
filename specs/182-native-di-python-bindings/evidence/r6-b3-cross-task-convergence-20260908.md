# R6-B3 CrossTask Convergence Audit

日期：2026-09-08。此批次执行 T015-A 的整体静态收敛审查，核对当前 native requester、
维护入口、Provider host、legacy manifest、collector/harness 与 T016 资格出口。审查是
只读的，没有启动产品构建、网络、MiniNDN 或 SIF/Tiger。

## Scope and allocation basis

批次共享一个审查出口：从 C++ `NativeInferenceClient::request` 和公开
`APPClient.request_native_payload` 到维护中的 YOLO/Qwen branches、`ServiceProvider`
registration，再对照 R5/R6 evidence、唯一 compatibility/case manifest 和 tasks/traceability
状态。覆盖五个 lane：

| Lane | Coverage | Evidence / remaining |
| --- | --- | --- |
| production entry/callers | covered | C++ requester, public facade, YOLO/Qwen native branches and Provider registration were located with CodeGraph/rg; default Python callers remain retained |
| implementation and wire | covered for recorded native owner/config and transaction contracts | R5 requester/conversation/provider evidence and current C++ symbols agree; real cross-process stream/recovery remains open |
| test/harness/oracle | covered for local selectors and registration | C++ focused suites, 344-entry compatibility manifest, R6-B1 collector tests and R6-B2 22-case registration are present; no full PO execution |
| build/source closure | covered by prior checkpoint identities; N/A for this read-only audit | baseline is `6171cf4d`; no source/header/build registration changed in R6-B3 |
| migration/evidence | gap remains by design | default public Python route, legacy zero-use, real node/netns/process cleanup and T016 qualification are not yet proven |

## Findings and ownership

没有新增跨任务控制性缺陷。审查确认的未闭合项均已有 owner：

- 默认 `distributed_inference`、ACK-driven callers 和 legacy provider/runtime/facade 仍有
  consumer；T013-B/T013-F 不能在没有 maintained caller zero-use 证据时删除旧 owner。
- `NATIVE_REQUEST_PIPELINE_NOT_READY` 是 native requester 缺少完整 runtime/configuration 时
  的显式 fail-closed 边界；它证明没有 Python fallback，不证明默认维护入口已迁移。
- R4/R5 的真实 Provider 两轮、stream callback cross-process delivery、conversation
  recovery/replacement 和 owner/config identity 仍需对应生产 selector 或 T016 资格记录。
- R6-B1/R6-B2 只关闭了 collector verdict 语义、manifest registration 和 fresh-run refusal；
  真实 namespace、短命 child、socket allow-list、cleanup 以及 I01--I08/PO outcomes 仍由
  T016 执行。

这些项目分别回到 T004/T008/T009/T010/T011/T013 或 T016；本审查不改设计契约、不放宽
依赖，也不把静态/局部测试提升为整体 PASS。

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `6171cf4d`
- Scope: `audit.md`, `traceability.md`, `tasks.md`, current requester/facade/caller/provider
  call paths, R5/R6 evidence and manifests
- Queries: CodeGraph for `NativeInferenceClient`, `APPClient.request_native_payload`,
  `ServiceProvider` registration and campaign owner; `rg` for maintained/native/legacy routes;
  status/manifest/dependency cross-checks
- Result: no new actionable control finding. Known migration and qualification gaps remain
  explicitly assigned and are the reason for `OPEN_FOR_NEXT_BATCH`.

## Validation

```text
codegraph explore "NativeInferenceClient APPClient request_native_payload ServiceProvider run_campaign"
# current source symbols/call relationships returned; broad temporary-tree results excluded

python3 -m py_compile Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
# exit 0; source unchanged in this audit

python3 specs/182-native-di-python-bindings/checklists/validate_design.py
# ok=true

git diff --check
# exit 0
```

No native build was applicable. No runtime qualification was attempted. The audit does not
close T015-A globally because its required production evidence is still owned by the unfinished
caller/provider cards and T016.

## Result and closure

`STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS`.

Closure decision: `OPEN_FOR_NEXT_BATCH`. T015-A has a current, repository-authoritative map of
remaining production-chain gaps; after the corresponding owners produce real evidence, rerun this
cross-task review before T016 final qualification and T017 handoff.
