# R10-B26 Missing `REPO_REF` Negative Recheck

**Status**: DONE for the bounded missing-reference runtime recheck; cross-process execution and T016 remain PARTIAL/UNQUALIFIED  
**Date**: 2026-09-09  
**Baseline**: `aa022c84`  
**Owner**: existing `integration-tests` binary and `Spec170NativePostSelection` selector

## Scope and stable exit

R10-B26 closes the stale failure-log boundary from R10-B6 by rerunning the missing encrypted
`REPO_REF` object with the current Provider fixture. No source or binary rebuild is introduced.
The stable exit is a real Provider handler failure before runner input or successful Response,
with the selector completing inside its bounded fetch budget.

## Minimum Review Record

Review path: `/home/tianxing/.codex/skills/review-agent/SKILL.md`  
Review SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`  
Review baseline: `aa022c84`; diff scope is this evidence, the failure index and the R10-B26
plan/tasks records. The existing source/test implementation was inspected without modification.

| Lane | Status | Files / symbols and boundary |
| --- | --- | --- |
| `production entry/callers` | covered | `runNativeIngressCase` → `ServiceProvider::CollaborationContext::fetchEncryptedLargeData` → `ctx.fail` |
| `implementation and wire` | covered | `REPO_REF` v2 envelope, missing `REQUEST-LARGE` object, scoped `NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=1000` |
| `test/harness/oracle` | covered | `Spec170NativePostSelection/ProductionIngressRejectsMissingNativeRepositoryReference` and status/response assertions |
| `build/source closure` | covered | existing `.codex-tmp/spec182-r4-b2/build/integration-tests` target and registered `ndnsf-di-core-flow.t.cpp` source |
| `migration/evidence` | gap | maintained callers, cross-process transport, I02--I08 and T016 qualification remain open |

## Validation record

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  .codex-tmp/spec182-r4-b2/build/integration-tests \
  --run_test=Spec170NativePostSelection/ProductionIngressRejectsMissingNativeRepositoryReference \
  --log_level=test_suite
exit=0; testing time=1.946850s; *** No errors detected
```

The selector entered the real `Spec170NativePostSelection` case and passed its
`statusFailed=true`, `responseReceived=false`, `providerInputMatches=false`, and
`timedOut=false` assertions. The observed status reason is the expected
`failed to fetch native DI request input reference: ...`; no runner input was produced.
The raw selector log and return code are retained under
`.codex-tmp/spec182-r10-b26-missing-repo-ref-20260909/`. A post-run `vmstat 1 2` recorded
second-sample `si=28`, `so=0`; no build was needed, and the next native build remains subject
to the normal fresh resource check.

## Batch Retrospective

- `static`: source and selector ownership were rechecked; no new finding and no source change.
- `compile/link`: not applicable; the existing binary was intentionally reused.
- `runtime/test`: the previously recorded fixed-pump/default-timeout boundary now passes with the
  scoped 1 s missing-object budget; the complete assertion path exits 0.
- `unobserved`: requester/provider cross-process transport, maintained YOLO/Qwen execution,
  stream/conversation delivery, legacy retirement and T016 qualification.

## Closure decision

`CLOSED_FOR_VALIDATION` for the missing-object negative recheck. The older R10-B6 failure-log
entry is superseded by this current pass; it remains retained as the first failure boundary.
This result does not promote any parent task or final qualification status.
