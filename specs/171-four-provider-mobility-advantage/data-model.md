# Data Model: Four-Provider Mobility Advantage

## CampaignProfile

| Field | Type | Validation |
|---|---|---|
| `campaign_id` | string | unique result namespace |
| `provider_ids` | list[string] | exactly four for primary campaign |
| `ap_layout` | list[AP] | one or three APs |
| `coverage_range_m` | number | positive; registered in cell manifest |
| `speed_mps` | number | one of 2, 8, 15 for formal bands |
| `seed` | integer | retained and included in trace hash |
| `mobility_warmup_s` | number | deterministic pre-trace RandomWaypoint burn-in; recorded in manifest and trace metadata |
| `rate_rps` | number | 5 for formal cells |
| `duration_s` | integer | 60 for formal cells; under 10 for smoke |
| `global_deadline_s` | number | 5 for formal cells |
| `attempt_timeout_s` | number | 0.2 for formal cells |

## ProviderTraceRow

| Field | Type | Validation |
|---|---|---|
| `time_s` | number | monotonic epoch time |
| `provider_id` | string | one of four registered Providers |
| `x_m`, `y_m` | number | inside declared area |
| `nearest_ap` | string | registered AP or empty when out of all ranges |
| `distance_m` | number | distance to nearest AP |
| `in_range` | boolean | derived from coverage range |

## CellSummary

| Field | Type | Meaning |
|---|---|---|
| `system_id` | enum | `ndnsf`, `grpc`, or `nsc` |
| `logical_requests` | integer | offered logical operations |
| `success` / `deadline_failures` | integer | terminal outcomes |
| `attempts` / `failovers` | integer | explicit recovery cost |
| `p50_ms`, `p95_ms`, `p99_ms` | number | successful-request latency |
| `recovery_gap_ms` | number | outage-to-success gap distribution |
| `provider_executions` | map | execution count per Provider |
| `trace_sha256` | string | paired-trace identity |
| `status` | enum | passed, failed, interrupted |

## Relationships

`CampaignProfile` produces one or more `ProviderTraceRow` groups. A
`SystemCell` binds one profile and one trace to one system. Each cell emits one
`CellSummary` and one immutable evidence manifest. Cells sharing a trace hash
are paired observations; requests inside one cell are clustered, not
independent experimental samples.
