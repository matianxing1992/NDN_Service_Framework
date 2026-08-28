# Spec 125 Completion Summary

**Status**: complete; the original frozen network cell remains a retained
negative result and `confirm06` is the accepted post-fix 60-second cell.

## Delivered behavior

- Mapping v2 signs sample group ID, opaque class, item position, predicted
  source extent, and repair extent while semantic Data names remain the real
  Interest targets. Mapping v1/v2 sessions cannot be mixed.
- Core provides C++ and Python `announceSample`, optional
  `prepareSampleExtent`, `publishSample`, and adaptive-open APIs. A bounded
  predictor learns each APP-defined class independently.
- The consumer schedules complete predicted groups, follows exact future
  Mapping blocks across unpublished frontiers and trailing tombstones, and
  never lets pressure shrink capacity below the next admissible atomic group.
- Authenticated actual extent corrects prediction; variable FEC uses only real
  source items. UAV publishes real H.264 access units without empty padding.
- UAV stream-session validation is separated from mutable descriptor-checkpoint
  validation, so retention/frontier movement cannot invalidate historical Data
  names or name-bound AEAD.
- A class seed is now only a cold-start estimate. Once authenticated history
  exists, prediction is the bounded same-class maximum plus margin rather than
  being permanently clamped by the seed. UAV's stable fixed encoder uses zero
  margin; unprecedented growth still fails visibly as underprediction.

## Retained original negative evidence

The original cell remains at
`results/spec125-adaptive-sample-atomic-20260719-acceptance/`. It completed the
60-second window but returned 1 because the GUI decoded zero frames. Frame 1
failed closed with `invalid UAV stream cursor frontiers:
invalid-frontier-order`: ahead Mapping had advanced while the provisional UAV
descriptor still held an old frontier. Its first sample nevertheless proved
prediction 4, actual sources 1, repair 1, retained items 2, empty padded sources
0, and no underprediction. The result was never overwritten or reclassified.

## User-authorized defect-closure history

All confirmation cells retained the zero-loss, 1200-kbit/s, 320-pixel,
one-repair, 60-second workload and used unique result directories.

- `confirm01`: descriptor/session coupling still rejected publication.
- `confirm02`: publication continued and GUI decoded 7 frames, then stopped at
  the initial Mapping horizon.
- `confirm03`: GUI decoded 80 frames; trailing tombstones failed to arm the next
  unpublished Mapping block.
- `confirm04`: GUI decoded 237 frames; pressure could shrink payload capacity
  below the next atomic group.
- `confirm05`: continuous playback passed with 1833 frames, but the cold-start
  seed remained a permanent prediction floor and future-hit ratio was only
  44.5583%.
- `confirm06`: PASS. GUI decoded 1832 frames with zero frame gap; Provider
  future hits were 3294/3311 (99.4866%); 3689 payload Interests served 3672
  actual source-plus-repair items (0.463% overhead); both class predictions
  converged to one source with zero underpredictions; decoder startup was 43
  ms. A shared-host capture-to-decode sample (59 every-30th-frame records after
  warm-up) measured p50 111.341 ms, p95 124.586 ms, p99 126.538 ms, and max
  127.850 ms. Raw logs and `acceptance-summary.json` are retained in
  `results/spec125-adaptive-sample-atomic-20260719-confirm06/`.

The accepted command was:

```bash
NDNSF_TIMELINE_TRACE=1 NDNSF_TIMELINE_TRACE_SAMPLE_RATE=0.05 \
NDNSF_STREAM_PACKET_TIMELINE_TRACE=1 \
NDNSF_UAV_VIDEO_PIPELINE=gstreamer \
NDNSF_UAV_GSTREAMER_SOURCE=videotestsrc \
sudo -n -E timeout 210s xvfb-run -a /usr/bin/python3 \
  Experiments/NDNSF_UAV_GUI_Minindn.py \
  --drone-headless --camera-mode file --no-virtual-camera \
  --flight-controller-backend mock --no-start-jmavsim --no-cli --no-xhost \
  --nfd-log-level WARN --video-bitrate-kbps 1200 --video-width 320 \
  --video-fec-parity-shards 1 \
  --live-stream-prefetch-policy adaptive-sample-atomic \
  --output-dir results/spec125-adaptive-sample-atomic-20260719-confirm06 \
  --auto-video-test --auto-stop-seconds 60 --auto-start-delay-ms 1000
```

MiniNDN reported its dummy key-chain patch. The live cell therefore proves
network/runtime behavior; signatures, Validator ownership, AEAD, malformed
Mapping rejection, and secret-leak invariants are covered by deterministic
security gates rather than this cell.

## Final verification

- `./waf build -j4` — PASS. A prior high-parallelism link attempt hit a linker
  SIGSEGV; the bounded retry passed without source changes.
- `./build/unit-tests --log_level=message` — 333/333 PASS; optional external
  ONNX model cases reported their documented skips.
- `./build/unit-tests --run_test=Stream,UavProtocolState --log_level=message`
  — 107/107 PASS.
- `PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py`
  — 19/19 PASS.
- `PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_uav_unified_video.py`
  — 12/12 PASS.
- `python3 tests/run_uav_stream_security_contract.py` — PASS, including the
  same 107 C++ cases and all 12 static/security checks.

## Success criteria

- SC-001, SC-002, SC-003: PASS in deterministic group, predictor, pressure,
  Mapping-boundary, variable-FEC, malformed-wire, and security tests.
- SC-004: PASS; real one-source groups plus one repair produced 1832 decoded
  GUI frames without synthetic empty sources.
- SC-005: PASS; key and delta histories converged independently with zero
  underprediction events.
- SC-006: PASS; Interest overhead 0.463%, future-hit success 99.4866%, sampled
  p95 124.586 ms/p99 126.538 ms, and continuous final-ten-second playback all
  satisfy the declared limits.

No Docker, iTiger, host-NFD final validation, or automatic bitrate/window
tuning was used.
