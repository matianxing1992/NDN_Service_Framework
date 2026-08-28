# Contract: Same-Binary Serial Sync-Production Offload Experiment

## 1. Command Surface

The implementation MUST provide these fail-closed commands:

```bash
python3 Experiments/build_svs_serial_production_offload.py \
  --base 6bb34545b4f89f1f6c265a68c18f1a40ade413eb \
  --boost-root <boost-1.71-root> \
  --output build/spec137-four-core

sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign <campaign-dir> \
  --preflight

sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign <campaign-dir> \
  --pilot

sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign <campaign-dir> \
  --seal

sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign <campaign-dir> \
  --run-formal

python3 Experiments/analyze_svs_serial_production_offload.py \
  --campaign <campaign-dir> \
  --verify \
  --report specs/137-svs-serial-production-offload/evidence/offload-proof-report.md
```

Before implementation these commands are interface contracts, not evidence
that the scripts already exist.

## 2. Runtime Binary Contract

```text
svs-serial-production-offload
  --production-mode {face-serial,worker-serial}
  --rate PPS
  --warmup SECONDS
  --measure SECONDS
  --drain SECONDS
  --cell-id ID
  --campaign-id ID
  --peer-id ID
  --remote-peer-id ID
  --events PATH
  --resources PATH
  --main-cpu CPU
  --face-cpu CPU
  --worker-cpu CPU
  --publish-enabled {true,false}
```

Unknown modes, missing fields, out-of-range CPU assignments, or configuration
that disagrees with the sealed manifest MUST fail before the ready barrier.

On startup each process emits one `runtime-config` record containing the full
expanded settings. The runner performs a field-by-field diff after deleting
only:

```text
production_mode
parallel_sync_production
production_workers
production_queue_capacity
sign_in_worker
build_extra_in_worker
worker_cpu_active
```

No other difference is admissible.

## 3. Event Contract

Every JSONL event includes:

```json
{
  "schema": "spec137.event.v1",
  "campaignId": "string",
  "cellId": "string",
  "peerId": "string",
  "phase": "startup|warmup|measured|drain|shutdown",
  "event": "stable-name",
  "monotonicNs": 0,
  "threadRole": "main|face|production-worker|supervisor",
  "logicalId": 0,
  "productionId": 0,
  "details": {}
}
```

Mandatory unsampled events:

```text
runtime-config
ready
phase-boundary
production-terminal-anomaly
production-fallback
signer-concurrency-max
worker-stats
shutdown-start
shutdown-complete
process-summary
```

Lifecycle details may be sampled at a fixed 1% by stable logical ID. Aggregate
counters are never sampled.

## 4. Thread And CPU Contract

- Face thread identity is recorded once and checked at every Face-owned stage.
- Production-worker identity is recorded once in `worker-serial`.
- `face-serial` MUST have no production-worker stage.
- `worker-serial` MUST execute extra-build, encode, and Sync sign on its one
  worker; snapshot/admission and finalization remain Face-owned.
- Per-stage wall and thread CPU clocks MUST be recorded from the stage's owner
  thread.
- Every Sync signing entry uses the common active/max signer probe.
- The four CPUs belong to the whole experiment, not to the worker. Both NFDs
  share CPU 0, the sole publisher pacer/Face share CPU 1, the fixed receiver
  uses CPU 2, and the sole publisher worker uses CPU 3 only in
  `worker-serial`.

Any owner mismatch is a terminal admission failure.

## 5. Fixed-Rate Diagnostic Contract

The only non-formal diagnostic order is fixed:

```text
60 face, 60 worker
```

Each cell runs once. The rate is pre-registered rather than selected from
results; the operator cannot supply `--formal-rate`.

`rate-selection.json` includes both diagnostic rows, fixed rate/reason, source
hash, binary hash, and contract hash.

## 6. Seal Contract

`--seal` succeeds only after all preflights and the fixed-rate diagnostic pass.
It writes:

```json
{
  "schema": "spec137.campaign.v1",
  "state": "sealed",
  "frozenRate": 0,
  "subjectManifestSha256": "hex",
  "binarySha256": "hex",
  "contractSha256": "hex",
  "topologySha256": "hex",
  "cpuMapSha256": "hex",
  "cells": [
    {"ordinal": 1, "pair": 1, "mode": "face-serial"},
    {"ordinal": 2, "pair": 1, "mode": "worker-serial"},
    {"ordinal": 3, "pair": 2, "mode": "worker-serial"},
    {"ordinal": 4, "pair": 2, "mode": "face-serial"},
    {"ordinal": 5, "pair": 3, "mode": "face-serial"},
    {"ordinal": 6, "pair": 3, "mode": "worker-serial"}
  ]
}
```

After sealing, build, pilot, topology, CPU map, rate, cell order, thresholds,
and analysis contract are immutable.

## 7. Formal Execution Contract

- One campaign lock owns formal mutation.
- Cells execute in ordinal order.
- Before each cell, NFD/process residue is checked and cleared only within the
  named MiniNDN experiment namespace.
- A two-peer ready barrier verifies routes, identities, binary hashes, and
  runtime configuration before measurement.
- A bounded watchdog terminates a hung cell and writes one failure receipt.
- A receipt ledger rejects a second attempt for an existing ordinal.
- Formal execution stops after an infrastructure failure that could contaminate
  later cells; already started cells remain immutable.
- Inadmissible scientific outcomes do not stop later cells unless they imply
  contaminated shared infrastructure.

## 8. Admission Contract

All must pass:

```text
same_source_tree
same_common_patches
same_binary
boost_1_71_only
same_runtime_controls_except_treatment
two_nodes_two_processes
routes_ready
cpu_affinity_valid
one_face_thread
receive_workers_zero
attempted_rate_within_2_percent
max_active_sync_signers_one
production_fallback_zero
production_accounting_remainder_zero
publication_accounting_remainder_zero
event_and_resource_files_complete
shutdown_drained
receipt_unique
```

Admission never deletes a cell. It marks the receipt admissible or
inadmissible with exact reasons.

`stale_sent` is a completed-work annotation and `stale_dropped` is a terminal
outcome. Both enter the outcome decision table; neither creates an unexplained
accounting remainder merely by being nonzero.

## 9. Analysis Contract

The analyzer:

1. verifies manifests and raw hashes before parsing;
2. emits one row per cell/peer and one combined row per cell;
3. constructs only the three registered pairs;
4. reports all primary and safeguard endpoints;
5. applies the ordered outcome table without operator override;
6. separates CPU, queue, callback residence, and network latency;
7. labels pilot data non-formal and never pools it with formal values;
8. never treats heartbeat or publication samples as independent replicates;
9. includes failed and inadmissible cells in the report; and
10. writes its own input/output hashes and version.

## 10. Protection And Rollback Contract

- The active NDN-SVS checkout and refs are unchanged before and after build.
- Spec 133/135/136 protected paths are hashed before work and at closure.
- The treatment changes no default NDN-SVS runtime behavior outside the
  standalone benchmark.
- The measurement patch is isolated and removable by deleting the Spec 137
  worktree/build after closure.
- No source patch is merged, rebased, or pushed as part of this experiment.
