# Real MiniNDN cancellation gate after DATA_V1 filtering (2026-08-19)

This record closes one protocol/debug loop for the host MiniNDN diagnostic
path. It is not a SIF or Tiger release gate; the current working tree was
dirty and the run used the rebuilt host diagnostic stack.

## Reproduction before the final fix

Run directory:
`/tmp/spec170-hybrid212-filterfix-062020`

Configuration:

```text
assignment=hybrid-212
role-execution-delay-ms=5000
ack-timeout-ms=8000
lease-timeout-ms=20000
spec111-fault=post-certificate-cancellation
data-v1-no-progress-ms=2000 (implicit old default)
```

The request-name/capability filter had already removed the earlier stale
segment mismatch, but the two-second group no-progress watchdog terminated the
current request before the delayed OP2 publication arrived. The provider log
reported `NDNSF_DATA_V1 group is terminal` for both the cancelled request and
the terminal-preservation request. This was a timing/configuration failure,
not evidence of stale SVS data after the filter fix.

## Changes under test

1. `ServiceProvider` now filters shared SVS catch-up by the current `REQ`
   field, and the DI dependency fetch additionally requires matching
   `REQ`, `ATTEMPT`, `PLAN`, `GROUP`, and `EPOCH` capability fields.
2. The static user driver and production `AutomaticPlanningCoordinator` now
   accept an explicit `data_v1_no_progress_ms` bound. The default remains
   2000 ms; injected-delay tests must opt in to a larger bound and record it.
3. The MiniNDN Spec111 assertion now checks the gate's actual
   `activatedProviders` field rather than the nonexistent `certifiedProviders`
   field. The former false-negative made a passing cancellation gate appear to
   fail.

## Final real run

Run directory:
`/tmp/spec170-hybrid212-noprogress12s-fixed-063549`

Command:

```bash
sudo -n env PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper:$PWD/Experiments:$PWD/pythonWrapper" \
  NDNSF_DI_TRACE_USER_STAGES=1 \
  python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --core-trace --assignment hybrid-212 \
  --requests 1 --concurrency 1 --role-execution-delay-ms 5000 \
  --out /tmp/spec170-hybrid212-noprogress12s-fixed-063549 \
  --enable-execution-leases \
  --spec111-fault post-certificate-cancellation \
  --cancellation-delay-ms 1000 --lease-timeout-ms 20000 \
  --ack-timeout-ms 8000 --data-v1-no-progress-ms 12000
```

Observed summary:

```text
status=SUCCESS
roleExecutionDelayMs=5000
dataV1NoProgressMs=12000
spec111Fault.status=passed
acceptedTerminalPreserved=true
acceptedTerminalRequestId=/native-tracer-2
stale cancellation: rejected
late cancellation of terminal request: rejected
replacement-attempt cleanup: accepted for the replacement epoch
```

The successful logs contain current-request `NDNSF_DI_DEPENDENCY_OBJECT` rows
for `native-tracer-2` on all three boundaries and no
`manifest capability binding mismatch` or `NDNSF_DATA_V1 group is terminal`
failure. The gate therefore covers cancellation, stale fencing, delayed
cross-Provider DATA_V1 publication, and terminal-result preservation in this
injected-delay scenario.

## Scope and next gate

This is a protocol-integrity result for the host MiniNDN path. It does not
qualify the current SIF, GPU execution, throughput, or performance optimum.
Before promoting the current source, rebuild a new source-bound SIF and rerun
the full C++/Python/in-process and real MiniNDN positive/negative gates. Keep
the normal 2000 ms bound for ordinary runs; use 12000 ms only when the
5-second injected stage-delay fault is explicitly part of the scenario.

