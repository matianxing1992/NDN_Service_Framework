# Experiment Contract

## Immutable Subject

```text
commit = 6bb34545b4f89f1f6c265a68c18f1a40ade413eb
binary_sha256 = c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac
boost = 1.71 only
modes = face-serial | worker-serial
worker_count(worker-serial) = 1
receive_workers = 0
face_threads = 1
publish_api = publishAsync
```

## Rate And Timing

```text
calibration_rates_desc = [1000, 800, 600, 400, 200]
calibration_timing = 5/15/5
qualification_timing = 5/15/5
formal_timing = 10/60/10
formal_order = AB/BA/AB
formal_cells = 6
retry_count = 0
```

## Cell Admission

```text
attempted_rate_error <= 0.02
delivery_ratio >= 0.99
max_active_sync_signers == 1
fallbacks == 0
production_accounting_remainder == 0
publication_accounting_remainder == 0
pending == 0
thread_owner_violations == 0
host_contamination == false
```

## Pressure Admission

```text
face_cpu_ns / completed_productions / wall_interval
  represents >= 10% of one CPU
OR
face_heartbeat_p99_ns >= 2_000_000
```

## Necessity Admission

```text
all_six_cells_admissible
AND face_cpu_relief >= 50% in >= 2 pairs
AND heartbeat_p99_improvement >= 20% in >= 2 pairs
AND delivery_ratio_harm <= 1 percentage point in every pair
AND delivery_p99_harm > 10% in fewer than 2 pairs
AND one signer, zero fallback, zero pending in every cell
```

## Fail-Closed Rules

- A cell directory is unique and may not pre-exist.
- A formal receipt ordinal may be appended once only.
- A started formal cell is never retried or replaced.
- Rate selection cannot read worker results.
- The analyzer cannot omit an unfavorable or inadmissible receipt.
- Spec 137 protected hashes must match before and after the campaign.
