# Spec 129 R1 Implementation Evidence

Date: 2026-07-21 CDT  
Formal result: `results/spec129-r1-20260721_183058`

## Build and deterministic validation

- `./waf build -j2`: PASS, 348 targets, 13m17.439s.
- `python3 setup.py build_ext --inplace --force` in `pythonWrapper/`: PASS;
  `_ndnsf.cpython-38-x86_64-linux-gnu.so` was compiled, linked, and copied in
  place.
- `./build/unit-tests`: PASS, 363/363.
- Spec 129 focused Python discovery: PASS, 32/32.
- Compatibility remediation checks: negative ACK 9/9 and isolated wheel/profile
  installation 1/1 PASS.
- Security regression entry points: HELLO auth, HELLO ACK payload, selective
  ACK/custom selection, NAC-ABE routing, token negative, targeted selection
  confidentiality, and secure selection status all PASS. The three legacy
  socket-based scripts were run with a temporary host NFD, which was stopped;
  the formal network result below uses MiniNDN.

The complete Python discovery ran 917 tests: 911 passed, 4 skipped, and two
frozen historical guards failed and were deliberately not rewritten:

1. Spec 109 candidate-lineage metadata expects a different hash for its frozen
   pre-implementation audit file.
2. Spec 127 asserts that current Core/binding sources still equal the accepted
   Spec 126 snapshot; Spec 129 intentionally changes those sources.

All five functional errors initially exposed by the full run were fixed
(`include_ack_context` compatibility and lazy/declarative `cryptography`
ownership), after which only these two historical guards remained. Full rerun
log: `/var/tmp/spec129-full-python-rerun-1784676423.log`.

## Exact-once MiniNDN confirmation

Command:

```bash
sudo -n -E python3 \
  Experiments/run_spec129_selection_gated_deployment_matrix.py \
  --output-root results/spec129-r1-20260721_183058
```

The single-writer runner invoked every frozen cell exactly once. It performed
no automatic retry and no cell was selectively rerun.

| Measure | Result |
|---|---:|
| Accepted cells | 12/12 |
| Formal invocations | 12 (one per unique cell) |
| Requests / ACKs | 12 / 36 |
| R1 reservations | 33 |
| Selected / not-selected decisions | 11 / 22 |
| Decision receipts | 33 |
| Terminal reservation releases | 33 |
| Release receipts / expiry fallbacks | 3 / 2 |
| Contention retries / exhaustion | 6 / 1 |
| Status queries / snapshots | 2 / 2 |
| Tamper / replay / stale rejects | 1 / 1 / 1 |
| Pipeline overlap | 15 ms |
| Packet plaintext matches | 0 |
| Duplicate executions | 0 in every cell |
| Spec 128 hash guard | unchanged, 2,118 files |

Wire attribution was measured from the executed path: REQUEST 8,388 bytes, ACK
99,505 bytes, Selection 54,912 bytes, and Response 936 bytes. Per-cell
completion latency ranged from 349.026 ms to 708.737 ms.

Canonical artifacts are `campaign-summary.json`, `campaign-manifest.json`,
`campaign-runs.csv`, `campaign-cells.csv`, the twelve cell summaries, process
logs, and packet captures under the formal result directory.

## Scope boundary

The formal cells all execute the real NDNSF/SVS path across a MiniNDN link with
three independent Provider processes. Scenario-specific loss, restart,
dependency, retry, and status adversaries are deterministic control-plane probes
inside the requester process; exhaustive malformed and state-machine behavior
is closed by the deterministic C++/Python suites. No UAV, codec, or workload
special case was added.
