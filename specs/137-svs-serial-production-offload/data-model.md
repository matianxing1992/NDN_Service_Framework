# Data Model: Serial Sync-Production Offload Proof

## FrozenSubject

| Field | Type | Constraint |
|---|---|---|
| `base_commit` | hex string | Exactly `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` |
| `base_tree` | hex string | Resolved from clean commit |
| `patched_tree` | hex string | One common tree for both modes |
| `boost_patch_sha256` | SHA-256 | Canonical Boost-1.71 patch |
| `measurement_patch_sha256` | SHA-256 | Common profiler/treatment patch |
| `compiler` | object | Path, version, flags |
| `boost` | object | Version, include path, library paths |
| `binary_sha256` | SHA-256 | Identical across all cells |
| `library_sha256` | SHA-256 map | NDN-SVS, ndn-cxx, Boost dependencies |
| `elf_build_id` | string | Recorded |
| `build_commands` | string array | Exact configure/build commands |

## RuntimeTreatment

| Field | `face-serial` | `worker-serial` |
|---|---:|---:|
| `face_threads` | 1 | 1 |
| `receive_workers` | 0 | 0 |
| `parallel_sync_production` | false | true |
| `production_workers` | 0 | 1 |
| `production_queue_capacity` | 0 | 4096 |
| `sign_in_worker` | false | true |
| `build_extra_in_worker` | false | true |
| `sync_interest_batching` | false | false |

All other expanded settings must compare byte-for-byte equal.

## ProductionLifecycle

| Field | Description |
|---|---|
| `campaign_id`, `cell_id`, `peer_id` | Ownership |
| `production_id` | Monotonic per-process identifier |
| `trigger_reason` | Publication, retransmission, reply, or other |
| `state_generation` | Snapshot generation |
| `trigger_ns`, `admit_ns` | Face timing |
| `worker_start_ns`, `worker_end_ns` | Optional worker interval |
| `result_post_ns`, `face_resume_ns` | Optional return interval |
| `send_start_ns`, `send_end_ns` | Face finalization |
| `snapshot_cpu_ns` | Face CPU |
| `queue_wait_ns` | Worker queue wait |
| `extra_cpu_ns`, `encode_cpu_ns`, `sign_cpu_ns` | Stage CPU |
| `face_finalize_cpu_ns` | Face CPU |
| `thread_role_by_stage` | Face or production-worker |
| `active_sync_signers`, `max_active_sync_signers` | Seriality evidence |
| `stale_disposition` | None, stale-sent, or stale-dropped |
| `terminal` | Completed, stale-dropped, failed, fallback, cancelled |
| `interest_wire_sha256` | Sampled identity |

## PublicationLifecycle

| Field | Description |
|---|---|
| `logical_id` | Stable sender-local publication ID |
| `scheduled_ns` | Absolute pacer deadline |
| `attempt_ns`, `api_return_ns` | Application-side publication timing |
| `sequence` | NDN-SVS sequence |
| `commit_ns`, `advertise_ns` | Local progress |
| `delivery_ns` | Remote completion |
| `payload_valid`, `duplicate`, `out_of_order` | Correctness |
| `path` | Piggyback, Mapping fallback, Payload fallback |
| `retry`, `timeout`, `nack` | Network outcome counters |

## FaceHeartbeat

| Field | Description |
|---|---|
| `scheduled_ns` | Absolute 1-ms deadline |
| `observed_ns` | Callback entry |
| `delay_ns` | `observed - scheduled` |
| `skipped_periods` | Deadlines skipped without catch-up burst |
| `phase` | Warmup, measured, drain |

## PilotObservation

| Field | Description |
|---|---|
| `candidate_rate` | Fixed at 60 pps for the sole publisher |
| `treatment` | Runtime mode |
| `attempted_rate_error` | Relative error |
| `delivery_ratio` | Remote delivered / peer committed |
| `heartbeat_p99_us` | Face responsiveness |
| `delivery_p99_us` | End-to-end tail |
| `mechanism_invariants` | Seriality/fallback/conservation flags |
| `jointly_admissible` | Computed after both modes |
| `face_stressing` | Contract rule result |

## SealedCampaign

| Field | Description |
|---|---|
| `campaign_id` | Unique immutable ID |
| `subject_manifest_sha256` | FrozenSubject binding |
| `rate_selection_sha256` | Pilot decision binding |
| `frozen_rate` | Formal rate |
| `cell_order` | Exact six-cell AB/BA/AB order |
| `topology`, `cpu_map`, `timing`, `payload` | Frozen controls |
| `admission_rules`, `outcome_rules` | Contract hashes |
| `single_writer` | Host/PID/start/lock identity |

## FormalCellReceipt

| Field | Description |
|---|---|
| `ordinal`, `pair`, `treatment`, `rate` | Matrix identity |
| `started_at`, `ended_at` | Wall-clock audit times |
| `terminal_status` | Completed, failed, timeout, crash, inadmissible |
| `exit_codes` | Both peers plus supervisor |
| `admission_checks` | Named pass/fail map |
| `artifact_hashes` | Raw evidence binding |
| `retry_count` | Must equal zero |
| `reason` | Stable terminal reason |

## RunMetrics

One row per formal cell and peer, plus one combined row. Required columns:

```text
attempted_pps, attempted_error_pct
committed, advertised, delivered, delivery_ratio
delivery_p50_us, delivery_p95_us, delivery_p99_us
face_heartbeat_p50_us, p95_us, p99_us, max_us
face_production_cpu_us_per_completion
worker_queue_wait_p50_us, p95_us, p99_us
worker_service_p50_us, p95_us, p99_us
max_worker_queue_depth
max_active_sync_signers
production_triggers, submitted, completed, stale_sent, stale_dropped
failed, fallback, cancelled
production_accounting_remainder
sync_interests, mapping_interests, payload_interests
retry, timeout, nack, mapping_fallback, payload_fallback
face_cpu_seconds, worker_cpu_seconds, process_rss_peak_bytes
```

## PairedContrast

| Field | Description |
|---|---|
| `pair` | 1, 2, or 3 |
| `metric` | Registered endpoint |
| `face_value`, `worker_value` | Run-level values |
| `absolute_difference` | Worker minus Face |
| `relative_difference` | `(worker-face)/face` |
| `threshold_pass` | Metric-specific practical rule |

## OutcomeClassification

| Field | Description |
|---|---|
| `classification` | Registered taxonomy value |
| `rule_path` | Ordered decision-table branch |
| `h1`, `h2`, `h3`, `h4` | Pass/fail/indeterminate |
| `admissible_cells` | 0-6 |
| `limitations` | Explicit excluded claims |
| `supporting_artifacts` | Hash-bound paths |
