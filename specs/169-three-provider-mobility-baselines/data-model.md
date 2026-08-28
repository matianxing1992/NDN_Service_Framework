# Data Model

## LogicalRequest

- `request_id`: stable across all Provider attempts
- `scheduled_at`, `deadline_at`, `completed_at`
- `initial_provider_index`
- `terminal_status`: success or failure
- `latency_ms`

## ProviderAttempt

- `request_id`, `generation`, `provider_id`
- `started_at`, `finished_at`, `timeout_at`
- `status`: success, timeout, Nack, unavailable, deadline, late
- `service_executed`: boolean

## HealthObservation

- `provider_id`, `observed_at`, `status`, `latency_ms`
- `fresh_until`

Only the gRPC baseline maintains HealthObservation records. NSC has no
persistent equivalent.

## MobilityScheduleRow

- `time_s`, `provider`, `x`, `y`, `distance_m`, `in_range`

## CampaignCell

- immutable system/range/workload/source identity
- exact command and output directory
- start/end timestamps and terminal process status
- request, attempt, failover, latency and control-operation metrics
- mobility trace hash and log hashes

## Campaign

- six-cell immutable schedule
- common parameter manifest
- one terminal row per scheduled cell
- aggregate JSON and CSV artifacts
