# Quickstart

```bash
python3 tests/python/test_ndnsf_uav_stream_control_isolation_campaign.py
./waf build --targets=UavGroundStationApp,unit-tests -j4
./build/unit-tests --run_test=UavProtocolState --report_level=short
python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec098-uav-control-state-loss05-current-final --reparse-existing
```

The reparse must not relaunch baseline repetitions.

Frozen treatment, run once:

```bash
python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec099-uav-initial-control-attribution-loss05-final \
  --workload-modes control-only --runs 5 --loss-percent 5 \
  --auto-stop-seconds 60
```

Accept when each run has terminal or named-unknown attribution, with no
duplicate command, unterminated state, lifecycle abort, or unsupported claim.

## Continuity Under Transient Service Capacity

If the execution host reports transient capacity, rate-limit, transport, stream, or
service-unavailable failure, preserve the current build/experiment checkpoint
and resume with bounded retry. Do not restart a completed build or replace an
experiment repetition. During Spec 099, a capacity notice arrived while the Waf
build was active; the original build session was polled to successful completion
and no experiment had started, so evidence integrity was unaffected.
