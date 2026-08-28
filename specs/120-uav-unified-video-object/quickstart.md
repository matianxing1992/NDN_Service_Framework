# Quickstart: Validate Unified Named UAV Video

This guide is executable after Spec 120 implementation. Use unique result directories and preserve failed/negative outcomes.

## 1. Build and deterministic gates

```bash
./waf build --targets=ndn-service-framework,unit-tests -j4
./build/unit-tests --run_test=Stream
./build/unit-tests --run_test=UavProtocolState
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_core_streaming.py
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_uav_unified_video.py
```

Required outcomes:

- published-packet feed yields immutable Mapping/source packet wires while the Face/I/O path executes no APP callback or Repo operation;
- one semantic name maps to one wire digest across live delivery, retention, restart, and replay;
- Mapping contains names/checkpoints only and remains under the signed wire cap;
- live/history grants recover the same authorized epoch keys for canonical packets but no plaintext secret reaches Repo, manifests, status, or logs;
- tampered name/wire/digest/signer/session/key grant/checkpoint fails before decoder admission;
- certificate rotation retains original signer/chain/time evidence and never causes packet re-signing or validation downgrade;
- queue overflow, storage failure, partial commit, and restart create explicit gaps or resume from the last durable checkpoint;
- a late recorder atomically obtains required Mapping plus future events and begins only at a decoder-safe H.264 join;
- optional repair remains transport-only;
- legacy raw recording DB is rejected clearly;
- no duplicate H.264 encode, media encryption, signing, raw recording writer, or recording-only decoder path remains.
- deterministic timelines use one stable sampled cursor set, preserve local
  monotonic ordering, name the post-FFmpeg event `encoded-output-ready`, and
  reject ambiguous, reversed, cross-session, or silently missing events.
- performance timeline events use only the dedicated sampled `NDN_LOG`
  TRACE/DEBUG category; static and runtime checks find no `std::cout`,
  `std::cerr`, `printf`, `fprintf`, or per-packet INFO timing path.

## 2. Static removal and secret gate

```bash
python3 tests/run_uav_stream_security_contract.py --secret-scan
rg -n 'recordRawChunk|recordSingleRawChunk|uav-camera-recording-chunk|recording-persistent-v1|recordingPlaybackChunks|decodeRecordingFromFetchedChunksAsync' \
  NDNSF-UAV-APP tests
```

The secret scan must pass. The `rg` command must return no active implementation or test dependency after migration; an explicitly labelled historical specification reference is allowed outside runtime/test paths.

## 3. Fresh MiniNDN acceptance

Run one fresh 60-second measured repetition for each required cell:

```bash
sudo -n -E python3 Experiments/NDNSF_UAV_Unified_Video_Minindn.py \
  --mode live-only --loss 0 --duration-seconds 60 \
  --output results/spec120-unified-video-live-only-candidate1

sudo -n -E python3 Experiments/NDNSF_UAV_Unified_Video_Minindn.py \
  --mode recording-only --loss 0 --duration-seconds 60 \
  --output results/spec120-unified-video-recording-only-candidate1

sudo -n -E python3 Experiments/NDNSF_UAV_Unified_Video_Minindn.py \
  --mode live-and-record --loss 0 --duration-seconds 60 \
  --output results/spec120-unified-video-combined-loss00-candidate1

sudo -n -E python3 Experiments/NDNSF_UAV_Unified_Video_Minindn.py \
  --mode live-and-record --loss 5 --duration-seconds 60 \
  --inject-storage-failure \
  --output results/spec120-unified-video-combined-loss05-storage-failure-candidate1
```

The launcher must reject a nonempty partial output directory. Do not rerun a failed cell under the same candidate identity.

Record per run:

- scheduled/terminal request and stream state;
- live and retained packet name/wire digest equality;
- encode and payload-encryption counts;
- mapping-published, Interest-arrived, payload-produced ordering and future-hit ratio;
- p50/p95 live delay, time to safe playback, timeout/Nack/failure counts;
- per-stage sample/missing counts and p50/p95/p99 for Provider processing,
  Consumer security/reorder/decoder work, and the truthful
  encoded-output-to-decoder-output interval;
- clock-domain IDs, offset uncertainty when cross-node subtraction is used,
  and RTT/causal ordering when it is not safe;
- retention lag, queue depth/high-water, write latency, bytes, checkpoint, gaps;
- Core/application pending maxima, NFD PIT, CPU and memory;
- validation/decryption/replay/secret-scan failures.
- late-retention requested/safe-start cursors and snapshot-to-feed continuity.
- tracing queue drops plus tracing-on/off p95 and CPU overhead.

At 0% loss, at least 99% of eligible source Interests must reach the Provider before production. If the lead gate fails, report the observed `3/2 RTT`-class discovery path; do not claim one-way prefetch.

Names-only Mapping is accepted and the inline candidate remains disabled when this gate passes. If bounded lead tuning still fails, evaluate a new version/candidate identity with exact canonical inner Data wires, independent inner/outer signature and binding negatives, recursion/downgrade rejection, signed wire-cap checks, ordinary semantic-name retrieval, and matched latency/bandwidth/cache reporting. Do not enable it merely because smaller segmentation makes the outer packet fit.

### Visible GTK presentation gate

The ServiceContainer decoder metric is not sufficient evidence that the GUI
shows video. Run one fresh automated GTK smoke with a visible display:

```bash
sudo -n -E python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --controller-node memphis --gs-node memphis --drone-node ucla \
  --drone-headless --camera-mode file --no-virtual-camera \
  --flight-controller-backend mock --no-start-jmavsim \
  --auto-video-test --auto-stop-seconds 30 --auto-start-delay-ms 1000 \
  --video-width 320 --video-bitrate-kbps 1200 \
  --video-fec-parity-shards 1 --live-stream-prefetch-policy mapped-pressure \
  --nfd-log-level WARN \
  --output-dir results/spec120-gui-visible-candidate1 --no-cli
```

The window must contain a nonblank video image and a GUI-owned decoded-frame
count of at least three. The ground-station log must contain
`AUTO_VIDEO_GUI_RENDER_GATE status=PASS`; the launcher treats its absence as a
failure even when the ServiceContainer decoder count is nonzero. Retain a
screenshot in the candidate directory.

## 4. Restart replay gate

After the recording-only and combined cells finish:

1. stop the original live consumer and recorder cleanly;
2. restart the replay producer and Ground Station;
3. retrieve the manifest and authorized key grant;
4. replay beginning-to-end through the normal LiveStream/UAV admission callback;
5. compare semantic names, signed-wire digests, Provider, ordering, and decoded H.264 digest with the captured live evidence.

Every committed source packet must match. Gaps must be surfaced; missing or altered packets must never reach the decoder.

## 5. Performance-attribution and optimization gate

Before changing a scheduling, Mapping, grouping, crypto, thread, or decoder
algorithm:

1. freeze the accepted baseline command, input, source/runtime digests, sampler
   version/rate, topology and candidate identity;
2. rank stage p95 values and queue waits, with missing counts and clock
   uncertainty visible;
3. change one owned concern and run five matched 60-second pairs at the
   declared loss level;
4. compare correctness/security first, then stage and end-to-end p95, CPU,
   queue, Mapping, timeout/Nack and completion metrics;
5. accept an optimization claim only when SC-016 passes; otherwise retain it
   as negative or inconclusive evidence.

Run a tracing-off/tracing-on matched check first. If production-rate tracing
changes p95 or CPU by more than the SC-015 bound, reduce the stable sampling
rate and create a new candidate identity before optimization experiments.

## 6. Adoption gate

Spec 120 is accepted only when correctness/security gates pass and combined retention does not reduce matched live completion. Latency, queue, or resource regressions are reported as measured outcomes. Hardware, real-radio, and long-duration claims remain deferred.

## 7. Accepted 2026-07-18 evidence

The accepted functional candidates are:

```text
results/spec120-accept-live-only-loss00-trace-off-20260718_200855
results/spec120-accept-recording-only-loss00-trace-off-candidate4-20260718
results/spec120-accept-live-and-record-loss00-trace-off-candidate5-20260718
results/spec120-accept-live-and-record-loss05-storage-failure-trace-off-candidate4-20260718
results/spec120-accept-late-start-loss00-trace-off-candidate1-20260718
results/spec120-accept-certificate-rotation-replay-loss00-trace-off-candidate2-20260718
```

The five accepted tracing pairs are the off/on `candidate1` directories named
`spec120-trace-correlated-v4-pair-01` through `pair-05`. The stable sample
denominator is 50 (2%). All ten runs passed and retained 100% eligible future
hits. Group-correlated p95 stayed within 5% in 5/5 pairs and CPU stayed within
5% in 4/5 pairs, satisfying SC-015. Pair 3's +7.91% CPU variation and all
superseded 20%, 10%, 5%, and pre-correlation 2% candidates remain preserved.
See `trace-matrix-summary.json` for the paths and exact values.
