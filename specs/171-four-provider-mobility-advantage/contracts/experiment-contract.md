# Experiment Contract: Four-Provider Mobility Advantage

## Cell command contract

Every cell manifest SHALL contain:

```json
{
  "schema": "ndnsf-four-provider-mobility-cell-v1",
  "system_id": "ndnsf|grpc|nsc",
  "provider_ids": ["ucla", "wustl", "uiuc", "arizona"],
  "ap_layout": [{"id": "ap1", "x_m": 200, "y_m": 200}],
  "coverage_range_m": 50,
  "speed_mps": 2,
  "seed": 40,
  "mobility_warmup_s": 300,
  "rate_rps": 5,
  "duration_s": 60,
  "global_deadline_s": 5,
  "attempt_timeout_s": 1.0,
  "ack_timeout_s": 1.0,
  "trace_sha256": "<sha256>",
  "source_hashes": {},
  "command": "<exact command>"
}
```

The manifest or campaign-level metadata must also identify the execution
backend (`local-minindn` or `tigercluster`) and the bounded preflight result.
Backend labels are part of the evidence boundary: a remote run is not silently
pooled with local runs when the backend changes timing, topology, or capacity.
The mobility campaign remains local-first; TigerCluster is only an agent-selected
option for a registered cell whose resource requirement cannot be met locally.
See [`docs/tigercluster-execution-policy.md`](../../../docs/tigercluster-execution-policy.md)
for the no-confirmation, no-unbounded-retry rule.

## Client fairness contract

- Clients receive only endpoint/prefix configuration and workload settings.
- RandomWaypoint burn-in is applied deterministically while generating the
  shared trace, before trace timestamp zero; its duration is recorded in the
  campaign manifest and trace metadata and is never exposed to clients.
- Clients do not read mobility trace files, availability state files, or AP
  coordinates during execution.
- `gRPC-SEQ-4` and NSC use at most one attempt per Provider per logical
  request and no proactive health-directed reordering.
- `gRPC-HC-4` may use proactive health probes only after an explicit opt-in;
  it is never the primary SC-005 baseline. The default cell is
  `gRPC-SEQ-4` with health routing disabled.
- The repository's current health probe is the custom
  `NDNSFBaseline/Health` application RPC, not an implicitly available
  `grpc.health.v1` service. If a standard health service is introduced, its
  registration and client service-config must be recorded in the manifest.
- Under `single-active-handoff`, `gRPC-HC-4` must not fail setup merely because
  an endpoint is currently outside coverage; it may begin with an incomplete
  prewarm set and must report probe and health-directed-selection counts.
- All attempts consume one common logical deadline.
- NDNSF selection and Provider execution are recorded without translating NDN
  packet counts into gRPC message counts.

## Summary marker contract

Each client emits exactly one terminal JSON marker containing at least:

```text
system_id, logical_requests, success, deadline_failures,
attempts, failovers, p50_ms, p95_ms, p99_ms,
provider_executions, trace_sha256, status
```

Smoke additionally emits `SMOKE_OK`. Missing or duplicate terminal markers
make a cell fail; the failed cell is retained and is not automatically rerun.

## Claim contract

The campaign may label a result `NDNSF_MOBILITY_ADVANTAGE` only when a
pre-registered paired condition satisfies SC-005 or SC-005a, with `gRPC-SEQ-4`
plus NSC-4 as the sequential baselines and the all-unreachable fraction,
retry/control cost, and latency reported. Otherwise the aggregate MUST use
`NO_DEMONSTRATED_ADVANTAGE` or `INCONCLUSIVE_MISSING_CELL`.

### Provider discovery claim

The campaign may state that NDNSF avoids client-side Provider endpoint
pre-registration only when the retained NDNSF runtime command contains no
Provider identities or addresses and a Provider omitted from the client's
configuration executes successful post-transition requests. The corresponding
static baseline command and endpoint count MUST be shown. The report MUST say
that gRPC/NSC can add dynamic discovery through an external resolver or
configuration-update control plane; the measured distinction is whether that
machinery is required by the evaluated application.

### Opportunity-window schema

Each row uses one stable `(condition, seed, logical_request_id)` key and records:

```text
publication_trace_s
publication_monotonic_s
applied_gate_state
initial_provider
reachable_providers
opportunity_state = NONE_REACHABLE | INITIAL_REACHABLE | SWITCH_REQUIRED
terminal_status
user_latency_ms | null
pre_execution_switch_cost_ms | null
failed_attempt_time_ms | null
attempts_or_provider_executions
failure_stage | null
```

Opportunity state is analysis-only and MUST NOT be passed to a client. A
service-side or Provider-side timestamp is a lifecycle proxy, not user latency.
All opportunity states and their request counts remain in the output.

For the seeds 72--81 confirmatory holdout, `publication_monotonic_s` and the
cell's applied `mobility_trace.csv` are authoritative. A row is paired only
when all three systems have a complete four-Provider gate state and agree on
the registered opportunity class. Requests observed during sequential gate
application are retained as `TRANSITION_BOUNDARY` controls and excluded from
the conditional latency estimate. Planned trace time alone is insufficient for
holdout classification.
