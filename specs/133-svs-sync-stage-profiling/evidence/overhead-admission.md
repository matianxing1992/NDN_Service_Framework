# Spec 133 Overhead Admission

> **Historical rejected admission, preserved as negative evidence.** This
> receipt belongs to the invalid cross-thread harness and was never used to
> admit the formal matrix. The corrected single-I/O-thread admission is
> `results/spec133-svs-sync-stage-profiling/preflight-io-01/overhead-receipt-corrected.json`.

## Verdict

`REJECTED` — the fixed three-arm 1000 publications/s-per-peer MiniNDN preflight
did not complete in any arm. No formal manifest was created and zero formal
cells were consumed.

## Frozen subject

- Base commit: `a9944019f76791773604999f00128057b9534ace`
- Clean head: `bf1e3e37f0c4c7a5a04d678f0fa439283ee46d2d`
- Profiled head: `e9913c9a957a214d699ab5eb0bc99684e06573c5`
- Profiling patch SHA-256: `a1a0526f31109669d0c341d26b44628b8f24eb6fb8cc2b3cd42510daa83eba72`
- Subject manifest SHA-256 bound by receipt: `95457478182cb602e8c9de7b8daf95e87cd191d19ebff83e17141fab48ead2b6`
- Stage count/sample rule: 81 / deterministic 1-in-100
- Build: compression disabled; Boost 1.71; synchronous `publish()`; no internal worker pool

## Exact command

```bash
sudo -n -E taskset -c 0-3 python3 \
  Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py overhead-preflight \
  --subject-manifest build/spec133/subject-manifest.json \
  --output results/spec133-svs-sync-stage-profiling/preflight-01
```

## Outcomes

| Arm | Subject | Peer return codes | Terminal state | Direct stderr evidence |
|---|---|---:|---|---|
| A | clean control | 134, 139 | `SUBJECT_FAILURE` | `malloc_consolidate(): invalid chunk size` |
| B | profiled binary, profiling disabled | 134, 139 | `SUBJECT_FAILURE` | uncaught `std::bad_alloc` |
| C | profiled binary, logging enabled | 134, 134 | `SUBJECT_FAILURE` | pthread priority assertion; corrupted heap |

The clean-control failure proves the profiling patch is not a necessary cause
of the failure. The common failure is consistent with the historical subject's
documented unsafe shared-container boundary, but this run does not isolate a
specific data structure or race; no stronger root-cause claim is made.

The receipt's zero rate/CPU fields are failure sentinels, not measured zero
throughput. Application events are flushed only at orderly process shutdown,
which the crashing peers did not reach.

## Authority

- Receipt: `results/spec133-svs-sync-stage-profiling/preflight-01/overhead-receipt.json`
- Per-arm receipts/logs: `results/spec133-svs-sync-stage-profiling/preflight-01/{receipts,cells}/`
- Frozen build identity: `build/spec133/subject-manifest.json`

Per FR-009, the rejection blocks sealing and forbids a formal five-cell run.
