# Quickstart: UAV Video End-to-End Latency

This guide records the completed validation order and its promotion decision.

## 1. Deterministic identity gate

Run the focused UAV protocol and Python analyzer tests. The fixture must include exact PTS round-trip, a dropped encoder frame, one-to-many output, reordered publication cursors, FEC repair symbols, restart, and a retired GUI callback.

Accept only when every displayed sample has one codec-proven source identity and every ambiguous case is rejected with a reason.

## 2. Media-backend capability gate

Probe the selected system GStreamer ABI and required plugins. Run a bounded pipeline under Xvfb using a deterministic 30 FPS source:

```text
videotestsrc -> low-delay H.264 access-unit appsink
appsrc -> H.264 parser -> software decoder -> frame appsink
appsrc -> H.264 parser -> software decoder -> GTK GL sink (separate direct-render probe)
```

Verify PTS preservation, access-unit alignment, first-frame/key-frame behavior, five start/stop cycles, and bounded teardown. If this gate fails, record the reason and retain `legacy-pipe`; do not begin broad integration.

## 3. Corrected provisional-code-default baseline

Use `Experiments/NDNSF_UAV_GUI_Minindn.py` with:

- deterministic 30 FPS source;
- fixed resolution, bitrate, GOP, FEC, mapped prefetch, link, and identities;
- provisional current POSIX/20 ms code default;
- Xvfb GUI and WARN NFD logs;
- sampled `NDN_LOG` timeline;
- warmup outside a 60-second measured window;
- one unique `results/spec122-baseline-*` directory.

The baseline is admissible only if it reports frame accounting and every valid stage, labels physical presentation unavailable when necessary, and does not add unrelated percentile values.

## 4. Ranked one-variable probes

Use baseline stage evidence to select candidates in this order:

1. persistent/prewarmed decoder if startup dominates;
2. authenticated key-frame/config readiness if join dominates;
3. access-unit packetization if burst/cadence and Interest amplification dominate;
4. timestamp-preserving GStreamer backend if attribution or pipe jitter dominates;
5. bounded latest-frame mailbox if queue age dominates;
6. direct sink if conversion/GUI stages dominate;
7. hardware decode only after the software path is correct.

Each probe changes one variable. Stop that family on a correctness, security, continuity, drop, Interest, CPU, memory, PIT, queue, or no-benefit gate.

## 5. Matched acceptance matrix

After exploratory probes select and freeze one candidate, create new confirmatory identities. `B` is the exact-identity-instrumented `legacy-pipe + stdio-batched` rollback configuration; `C` is the selected complete configuration. Execute `B→C, C→B, B→C, C→B, B→C`, then trace controls `OFF→ON, ON→OFF, OFF→ON, ON→OFF, OFF→ON`. Each cell runs once. Do not count Spec 121 or T005-T008 exploratory cells. Report paired absolute and relative differences plus median paired effect.

The default changes only if at least four pairs satisfy Spec 122 SC-004 through SC-010, including the hard 2-times Interest-work-per-produced-frame bound relative to `B`. A higher-work candidate may remain labelled experimental but cannot become the default.

No exploratory candidate reached this admission point. The exact GStreamer
future-on cell decoded 725 of 1830 produced frames, covered only 37.24% of the
sampled source identities, reached 88.96% future-hit ratio, and used 14.091
Mapping-plus-payload Interests per decoded frame versus 4.900 for the rollback
reference. Therefore the required confirmatory pairs were intentionally not
created. This is the specified negative branch, not missing evidence.

## 6. Final regression

Run focused unit/protocol/security tests, Xvfb GUI start/stop, zero-loss acceptance, controlled loss with FEC on/off, latest join, reconnect, and headless mode. Preserve failed and negative cells. Update the UAV README and slide only with measured claims.

## 7. Rollback

Select `legacy-pipe` explicitly. For the current provider batching rollback, use:

```bash
NDNSF_UAV_ENCODER_PIPE_READ_MODE=stdio-batched
```

No measured cell may change backend or fallback after startup.

## 8. Canonical outcome

The default remains `legacy-pipe`. The GStreamer path is available only as an
explicit experimental-negative backend:

```bash
NDNSF_UAV_VIDEO_PIPELINE=gstreamer
NDNSF_UAV_GSTREAMER_SOURCE=videotestsrc
```

The exact candidate measured capture-to-widget p95 1053.178 ms, of which
capture-to-decode was 1052.030 ms and decode-to-widget was 8.797 ms. Physical
scan-out was unavailable. See `baseline-report.json`, `completion-summary.md`,
and the referenced unique `results/spec122-*` directories. Do not rerun or
replace these frozen cells.
