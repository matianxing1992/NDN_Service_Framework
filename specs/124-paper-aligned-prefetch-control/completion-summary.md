# Spec 124 Completion Summary

## Outcome

Spec 124 is complete. NDNSF's mapped live prefetch controller now follows the remaining relevant control semantics in Gusev et al., *Real-Time Streaming Data Delivery over Named Data Networking*, while retaining NDNSF's semantic names and stronger security contract.

The accepted evidence is:

```text
results/spec124-paper-control-20260719-064100/candidate-gstreamer-future-on
```

This was the first and only Spec 124 MiniNDN acceptance cell; it was not replaced or rerun.

## Defects found and repaired

1. **Detection hold bypass**: stable observations could move CHASING to ADJUSTING before the configured detection period expired. All phase/window changes now respect the hold, and `holdMs` uses the current decision clock rather than the last sample time.
2. **Wrong over-adjustment recovery**: when withholding first produced stale arrivals, the controller returned to CHASING and could double again. It now remembers and restores the previous usable `lambda_p` and enters FETCHING.
3. **Restored window was discarded**: FETCHING previously collapsed unconditionally to theoretical packet demand, which would erase the restored `lambda_p`. It now preserves the larger validated live window while still honoring increased network demand.
4. **Generation wait polluted RTT**: every payload expression-to-reception interval was fed directly into network RTT, although an early Interest measures `DRD' = DRD + dgen`. Known-produced Data remains direct evidence. Ahead-of-join observations cannot raise network RTT during CHASING/ADJUSTING; after stable FETCHING minimizes `dgen`, normal adaptation resumes.
5. **Misleading consumer metric**: `futurePayloadInterests` is based on the immutable join checkpoint, not current Provider production state. Its structure field remains compatible, but source comments, private naming, and UAV logs now call it ahead-of-join. Provider future-interest/hit counters remain authoritative.
6. **Impossible Interest-work gate**: the acceptance runner allowed only 12.65 Interests per frame although the configured payload contract itself is 12 source plus one repair Data. The explicit gate is now `13 * 1.15 = 14.95` including Mapping overhead.

## MiniNDN result

| Gate | Result |
|---|---:|
| Terminal status | PASS |
| Decoded frames | 1,832 |
| Decoder startup | 17 ms |
| Five-second buckets | all 12 active |
| Final ten seconds | active |
| Capture-to-decode p50/p95/p99 | 141.850 / 180.715 / 195.973 ms |
| Decode-to-widget p95 | 2.522 ms |
| Exact identity coverage | 171/172 = 99.419% |
| Provider future hits | 6,357/6,357 = 100% |
| Interest work | 13.438 per decoded frame |
| Consumer delivered items | 22,021 |
| Security failures | 0 |
| Terminal Provider pending / consumer in-flight | 0 / 0 |

Spec 123's accepted p95 was 205.946 ms; this run measured 180.715 ms. This is a useful non-regression result, not a statistical performance-improvement claim, because each candidate has only one preserved run.

## Verification

- Targeted build: `./waf build --targets=unit-tests,UavDroneApp,UavGroundStationApp,App_ServiceController -j2` passed before MiniNDN.
- Focused C++: Stream 43/43 and UavProtocolState 58/58 passed.
- Full C++: 327/327 passed.
- Python: latency analyzer 8/8 and prefetch campaign 3/3 passed through `unittest`.
- MiniNDN: all nine acceptance checks in `matrix-summary.json` passed.
- Cleanup: no NFD, MiniNDN, controller, drone, or ground-station process remained after the run.

The system Python did not provide `pytest`; the same two standard-library `unittest` suites were run directly and passed. The optional agent-context hook script referenced by the installed skill was absent, so its required effect was applied directly by updating the managed Spec Kit block in `AGENTS.md` to this plan.

## Paper alignment and deliberate differences

The implementation now preserves exact early Interests, a real phase-driven pipeline, detection holds, restore-after-over-adjustment, and network-only demand estimation. The paper proposes producer-provided `dgen`; NDNSF does not add consumer-specific timing to immutable cacheable Data, and instead uses the conservative phase-aware rule above.

The paper's full playable jitter buffer, `gamma * DRD` retransmission checkpoint, quality/bitrate challenge, and codec-specific key/delta estimators are not claimed as Core features. Buffer sizing and decode/playout remain APP-owned; a later loss/jitter matrix should decide whether Core needs additional generic recovery timing.
