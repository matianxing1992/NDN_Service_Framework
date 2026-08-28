# Implementation Plan: DI Closed-Loop Workload Campaign

**Branch**: `Experimental` | **Historical feature**: 012 | **Audit date**: 2026-07-21

**Spec**: [spec.md](spec.md)

## Summary

Extend the synchronous NativeTracer path from one request to a positive number
of sequential requests within one ServiceUser session. Emit one result per
attempt and one aggregate workload record; thread request count and metrics
through the MiniNDN harness and layout campaign. Preserve the single-request
default and explicitly exclude concurrent/open-loop work.

## Technical Context

**Language/Version**: Python 3; existing C++ native Provider target is built but
not changed by this feature

**Primary Dependencies**: Python NDNSF binding, NativeTracer driver, MiniNDN
harness, existing layout campaign runner

**Storage**: JSON/CSV run summaries; historical original output was under `/tmp`

**Testing**: `py_compile`, deterministic Python workload/parser tests, targeted
native build, historical MiniNDN smoke/campaign record

**Target Platform**: Linux and MiniNDN

**Project Type**: Experiment driver/harness/campaign extension

**Performance Goals**: Measure rather than promise makespan, p50/p95, and
successful-request throughput for a short sequential workload

**Constraints**: One outstanding request, same ServiceUser session, default one
request, no C++/wire/provider/model changes, no rerun or strengthened claim
without a new durable evidence plan

**Scale/Scope**: Historical smoke with 3 requests; historical campaign with 2
runs for each of 2 layouts

## Constitution Check

| Principle | Result | Evidence |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Uses the existing generic collaboration path; adds no generated stubs, split service names, Direct aliases, or wire types. |
| Security Is Part Of Data Path | PASS | Reuses the existing ServiceUser/security path without bypass or protocol change. |
| CodeGraph First | PASS | Audit verified current driver/harness/campaign symbols before changing documents. |
| Spec-Driven Durable Work | REMEDIATED | Historical free-form documents were upgraded to current Spec Kit structure. |
| Verify With Right Scope | CONDITIONAL | Current deterministic code can be checked; original `/tmp` MiniNDN artifacts are unavailable and cannot be promoted to verified evidence. |
| Cohesive Outcome-Based Tasks | REMEDIATED | Eight mechanical W-tasks were replaced by three cohesive story tasks. |

No implementation waiver is required. The evidence limitation is explicit and
controls the final audit verdict.

## Architecture and Data Flow

```text
user_driver.py
  --requests N (default 1; positive)
  start one ServiceUser
  for request in 1..N:
      submit synchronously
      emit NDNSF_DI_NATIVE_TRACER_USER_REQUEST
      stop on first failure
  summarize attempted results
  emit NDNSF_DI_NATIVE_TRACER_USER_WORKLOAD
  emit backward-compatible NDNSF_DI_NATIVE_TRACER_USER_EXECUTION

NDNSF_DI_NativeTracer_Minindn.py
  pass --requests
  parse aggregate
  retain requestCount/counts/latency/makespan/throughput in summary.json

run_layout_campaign.py
  pass identical request count to each layout run
  retain attributable campaign-runs.csv rows
  aggregate per-layout workload metrics
```

## Metric Contract

- `requestCount`: attempted results present in the workload summary.
- `successCount` / `failureCount`: status-derived attempted outcomes.
- `makespanMs`: elapsed workload interval around sequential submissions.
- `meanMs`: arithmetic mean of attempted request elapsed times.
- `p50Ms` / `p95Ms`: nearest-rank percentile of attempted request elapsed times.
- `throughputRps`: successful requests divided by measured workload seconds;
  zero when the interval is non-positive.
- Unsent requests after an early failure are not counted as attempted.

## Ownership and Files

| Behavior | Owner/source | Validation |
|---|---|---|
| Sequential loop and workload summary | `examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py` | focused driver/metric fixtures and syntax check |
| MiniNDN argument/summary propagation | `Experiments/NDNSF_DI_NativeTracer_Minindn.py` | parser/command tests and smoke contract |
| Layout row/aggregate propagation | `examples/python/NDNSF-DistributedInference/native_di_tracer/run_layout_campaign.py` | campaign aggregation fixtures |
| Native Provider availability | existing `di-native-provider` build target | targeted build only; no C++ change |

## Validation and Evidence Levels

1. **Implemented**: current source exposes positive `--requests`, workload
   summary, MiniNDN fields, and campaign fields.
2. **Tested-current**: only commands actually rerun during this audit may be
   labeled current PASS.
3. **Historically reported**: exact 2026-era smoke/campaign values retained in
   `spec.md`; raw `/tmp` artifacts are absent.
4. **Not claimed**: concurrent queueing, open-loop offered load, steady state,
   statistical significance, real GPU/model performance, or production results.

## Migration and Rollback

The feature is additive: `requests=1` remains the default. Disabling multi-
request use requires no persisted-state migration. If aggregate parsing fails,
the safe rollback is to one request and the legacy execution marker; do not
silently fabricate workload fields.

Later concurrent/open-loop code must preserve the explicit closed-loop mode and
metric definitions but is outside this historical feature.
