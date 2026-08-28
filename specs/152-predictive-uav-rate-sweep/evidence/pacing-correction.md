# UAV Capture Pacing Correction

## Red evidence

Command:

```text
./build/unit-tests \
  --run_test=UavProtocolState/UavGStreamerFileCaptureHonorsWallClockFrameRate
```

Before the correction:

```text
configured 10 fps -> achieved 256.238 fps
configured 60 fps -> achieved 1171.639 fps
result: FAIL (2)
```

This confirms that `videorate`/FPS caps alone assigned timestamps but did not
pace non-live file consumption while capture `appsink` used `sync=false`.

## Correction

Only the capture graph changed:

```text
appsink ... sync=false -> sync=true
```

The decode graph remains unsynchronized. No Core, binding, protocol, Mapping,
prefetch, retry, FEC, signature, or encryption behavior changed.

## Green evidence

```text
./waf build --targets=unit-tests,UavDroneApp,UavGroundStationApp -j2
./build/unit-tests \
  --run_test=UavProtocolState/UavGStreamerFileCaptureHonorsWallClockFrameRate
./build/unit-tests --run_test=UavProtocolState
```

Results:

```text
affected build: PASS
pacing test: 1/1 PASS
UavProtocolState suite: 71/71 PASS
Spec 152 Python runner/analyzer tests: 3/3 PASS
```
