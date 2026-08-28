# Quickstart

This is the execution order after implementation is explicitly authorized.

## 1. Verify frozen baselines without running them

```bash
for d in \
  specs/125-adaptive-sample-atomic-prefetch \
  specs/126-loss-reorder-resilience \
  results/spec125-adaptive-sample-atomic-20260719-confirm06 \
  results/spec126-loss-reorder-20260720-confirmation07
do
  rg --files "$d" | sort | xargs sha256sum | sha256sum
done
```

Compare with `contracts/frozen-baseline-contract.md`. Do not invoke a Spec 125
or Spec 126 runner.

## 2. Run deterministic focused tests

Use the exact target names established during T002/T003. The minimum required
coverage is:

```text
20/30/60 fps x legacy bounded-opaque/GStreamer exact-class inputs
capture/decode x std/non-std callback exceptions
unavailable/active/stale/replaced Core decision snapshots
VideoAdaptiveState field round trip
```

## 3. Run existing focused regressions

```bash
./build/unit-tests --run_test=Stream,UavProtocolState --log_level=message
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_uav_unified_video.py
python3 tests/run_uav_stream_security_contract.py
```

T004 must record the exact build command and any additional focused pipeline
test target introduced by the implementation.

## 4. Run one fresh MiniNDN acceptance cell

The runner/command is frozen in T004 after preflight. It must:

- use a new `results/spec145-uav-video-runtime-<fresh-id>/` directory;
- run two nodes, GStreamer, 20 fps, zero loss, and a 60-second measured window;
- execute once;
- retain failure;
- never write under a Spec 125/126 path.

## 5. Close and conditionally update Spec 144

Recompute all four frozen hashes, run the post-implementation audit, and
evaluate every success criterion. Update Spec 144 only if the audit verdict is
PASS.
