# Deployment-Fidelity Evidence Schema

## Raw lifecycle event

Every event is append-only JSONL with these common fields:

```json
{
  "schema": "ndnsf-di.lifecycle-event.v1",
  "experiment_id": "...",
  "request_id": "...",
  "attempt_epoch": 1,
  "event_id": "...",
  "event_type": "STAGE_EXECUTING",
  "component": "provider|user|repo|adapter|nfd|slurm",
  "provider": "/provider/0",
  "provider_boot_epoch": "...",
  "role": "stage-0",
  "plan_digest": "sha256:...",
  "operation_id": "...",
  "epoch": 1,
  "sequence": 7,
  "monotonic_ns": 0,
  "wall_time_utc": "...",
  "authenticated": true,
  "details_schema": "...",
  "details": {}
}
```

Fields not applicable to an event are null, not silently omitted by the
analyzer. Raw producer logs remain immutable.

## Required detail groups

- **ACK**: status, rejection reason, RTT/bandwidth estimate, capacity/load,
  residency inventory digest and entries.
- **Plan**: ACK-set, model, graph, partition, assignment, dependency and strategy
  digests; feasibility checks.
- **Repository**: manifest/data name, artifact digest, byte range, segment,
  retry/window/backlog, cumulative unique bytes, progress and terminal reason.
- **Residency**: prior/new tier, bytes, cache key, storage/device locator,
  verification/adapter evidence, reuse/eviction reason.
- **Stage**: direct dependencies, accepted input identities, backend/device,
  input/output shapes/digests, start/end, CPU fallback.
- **Token**: index, token ID, optional decoded fragment, first-token marker,
  generation timestamp; no new Request identity.
- **Terminal**: terminal kind/reason, complete answer, token count, security
  verdict, response digest and signer.

## Invocation summary row

One CSV/JSON row per scheduled invocation includes:

```text
experiment_id, schedule_row, prompt_id, request_id, attempt_epoch
model_identity, plan_digest, assignment_digest
cache_class (cold|disk|ram|gpu|mixed|invalid)
terminal_kind, terminal_reason, accepted, failure_class, failure_code
answer, answer_digest, token_count
request_to_ack_close_ms, planning_ms, publication_ms
artifact_fetch_ms, disk_to_ram_ms, ram_to_gpu_ms
dependency_wait_ms, execution_ms, response_ms, total_ms
ttft_ms, inter_token_latency_ms[], tokens_per_second
repo_unique_bytes, repo_wire_bytes, duplicate_model_payload_bytes
device_load_count, cpu_fallback_count
security_verdict, failure_class, evidence_path
```

## Campaign completeness

The analyzer must reconcile the frozen schedule with raw records. Every schedule
row is exactly one of success, classified failure, canceled, scheduler-not-
admitted, or environmental failure. Missing, duplicated, or silently replaced
rows fail the campaign.

## Causal assertions

The final analyzer verifies, rather than infers from log order alone:

- every stage start follows adapter-confirmed local GPU readiness;
- every non-root stage start follows accepted direct-predecessor output;
- no event references a different request/attempt/plan;
- no global-ready or fixed-settle event gates Stage 0;
- one terminal writer wins;
- warm reuse claims agree with transfer/load counters;
- complete answer/token ordering agrees with the terminal Response.

## Evidence bundle

Each formal run retains the immutable run manifest, Slurm job/accounting record,
node/GPU/process inventory, NFD route/strategy snapshot, security configuration
digest, raw per-component logs, lifecycle JSONL, schedule reconciliation,
invocation/campaign summaries, analyzer version, and a human-readable verdict.
