# T005 Fresh 20-fps Acceptance

## Build and regression

All commands completed successfully:

```bash
./waf build -j2
./build/unit-tests --run_test=Stream/*:UavProtocolState/* --log_level=message
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_uav_unified_video.py
python3 tests/run_uav_stream_security_contract.py
python3 tests/python/test_ndnsf_stream_latency.py
python3 Experiments/NDNSF_UAV_GUI_Minindn.py \
  --quick-smoke --video-fps 20 --camera-mode file --no-virtual-camera \
  --drone-headless --no-cli --no-xhost \
  --output-dir /tmp/spec145-launcher-preflight
```

Results:

- full Waf build: PASS;
- native Stream/UAV suite: 120/120 PASS;
- unified Python UAV video suite: 13/13 PASS;
- UAV streaming security contract: 12/12 checks PASS;
- latency analyzer suite: 9/9 PASS;
- 20-fps non-MiniNDN launcher preflight: PASS.

The generic Core/binding hashes remained exactly equal to their T001
baselines.

## Frozen formal command

Result root:

```text
results/spec145-uav-video-runtime-20260724T064253Z
```

The prepare step froze exactly one 20-fps, 60-second, GStreamer, zero-loss,
memphis/ucla MiniNDN cell. The command manifest SHA-256 is:

```text
09036e4b9d4512c601071da704fbdea2117ee6e7170be8ca495d68899c6d2038
```

The formal cell was executed once. `automaticRetry=false` and
`rerunAllowed=false` are recorded in both terminal artifacts.

## Measured result

Verdict: **PASS (1/1 formal cell)**.

| Metric | Result |
|---|---:|
| Encoded frame attempts | 1226 |
| Frame publications | 1226 |
| Delivered frames in the 60-second window | 1200 |
| Active five-second buckets | 12/12 |
| Class mismatches | 0 |
| GStreamer pipeline failures | 0 |
| Duplicate APP deliveries | 0 |
| Active Core callback observations | 119 |
| Provider future Interests / hits | 2445 / 2430 |
| Provider future-hit ratio | 99.3865% |
| Mapping Interests | 1389 |
| Mapping Data / new Mapping Data | 1229 / 1229 |
| Payload Interests | 2467 |
| Necessary source+repair items | 2452 |
| Payload Interest overhead | 0.6117% |
| Retry attempts | 158 |
| Timeouts | 21 |
| Nacks | 0 |
| Capture-to-decode samples | 1220 |
| Capture-to-decode mean | 158.609 ms |
| Capture-to-decode p50 | 156.006 ms |
| Capture-to-decode p95 | 168.687 ms |
| Capture-to-decode p99 | 181.718 ms |

The attempt/publication counter includes the bounded readiness interval around
the consumer's 60-second presentation window; the bucketed delivery count is
the measured-window value.

Every FR-017 acceptance check is true. Terminal evidence:

- `campaign-summary.json` SHA-256:
  `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566`;
- `zero-loss-20fps-run-01/run-summary.json` SHA-256:
  `6d842b6b538ef112a2d0a961fda129d950a9e546769a07974049841bd8055243`.

No Spec 125/126 runner was invoked and no historical result was written.
