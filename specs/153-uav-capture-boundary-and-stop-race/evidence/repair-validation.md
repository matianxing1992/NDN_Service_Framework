# Repair Validation

## Decode red/green

Live file capture was fed directly into the real decoder at 10 fps.

```text
before: median capture-origin -> decoder output = 400.423 ms (FAIL)
after avdec_h264 max-threads=1: 96.385 ms (PASS <=200 ms)
```

The existing requested-rate/capture-callback test still passes at 10/60 fps,
so capture pacing itself was retained.

## Asynchronous stop

The launcher now uses its existing bounded `wait_log(..., 5, drone_proc)`
before asserting the already-required Drone `video stopped` marker. No fixed
sleep or weakened assertion was introduced.

## Gates

```text
full ./waf build -j2: PASS
StreamFacade + UavProtocolState: 80/80 PASS
Spec 152/153 Python harness tests: 6/6 PASS
strict Spec 153 structure: PASS, 7/7 FR traced
successor prepare dry-run: PASS, six ordered cells
```

No generic Core, API, protocol, prefetch, Mapping, retry, FEC, or threshold
change is part of this repair.
