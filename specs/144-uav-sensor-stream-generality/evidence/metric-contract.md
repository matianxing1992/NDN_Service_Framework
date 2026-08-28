# T002 Generic Metric Contract

**Date**: 2026-07-24  
**Verdict**: PASS

## Authority and Scope

The metric authority is an unsampled, application-neutral terminal-attempt
ledger in `LiveStreamStatus`. Existing sampled `TimelineTrace` files remain
unchanged and are diagnostic only. No telemetry, acoustic, UAV, codec,
workload, profile, or formal-cell selector was added to Core or bindings.

## Exact Counters

Core and the native Python binding expose:

- Mapping Interests, validated Mapping Data, and Mapping Data that advanced
  resolver state;
- Payload Interests split by source/repair and initial/retry;
- provider-confirmed future Interests/hits split by initial/retry;
- source Data admissions, repair Data responses, and repair symbols consumed
  by successful recovery;
- application-useful, protection-only, nonproductive, and unresolved terminal
  Payload attempts;
- retry attempts/successes/suppressions/reasons, timeout, Nack, late arrival,
  deadline skip, exhaustion, and recovery counters.

The analyzer rejects a cell unless:

```text
Payload = Source + Repair + Unclassified
Source = InitialSource + RetrySource
Repair = InitialRepair + RetryRepair
ApplicationUseful = SourceAdmissions + RepairConsumed
Payload = ApplicationUseful + ProtectionOnly + Nonproductive + Unresolved
Unresolved = 0
```

Mapping novelty and provider-confirmed future-hit use separate denominators.
Zero denominators are explicit unavailable values and fail any controlling
gate. Latency uses arithmetic mean and nearest-rank p50/p95/p99/max under the
declared shared-host steady-clock domain.

## Verification

| Gate | Result |
|---|---|
| native Stream utility/recovery conservation cases | PASS |
| Python metric/neutrality suite | 8/8 PASS |
| source/repair split failure injection | PASS (fails closed) |
| unresolved/double-classification injection | PASS (fails closed) |
| Mapping/future denominator separation | PASS |
| exact interval fixtures | PASS |
| native binding properties | all new fields present |
| Core selector scan | zero prohibited executable selector |

This closes T002 without making sampled trace volume part of any formal
denominator.
