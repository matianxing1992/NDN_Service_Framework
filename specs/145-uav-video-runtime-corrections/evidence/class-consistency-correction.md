# T002 Class Consistency Correction

## Red

Command:

```bash
./waf build --targets=unit-tests -j2
```

The new `UavVideoSampleClassScheduleIsBackendTruthfulAtSupportedFps` test
failed to compile because the session-frozen sample-class schedule and backend
mode did not exist.

## Implementation

- GStreamer sessions use one exact `key`/`delta` schedule derived from the
  negotiated FPS and session generation for both future announcements and
  publication.
- A contradiction between that schedule and the actual GStreamer access-unit
  delta flag fails the session instead of silently changing class.
- The legacy byte-pipe backend uses one bounded `opaque` class because it does
  not preserve trustworthy access-unit class boundaries.
- The Core descriptor projection consumes the advertised class mode; it does
  not invent UAV/codec behavior in Core.

## Green

Covered by deterministic native tests:

- 20, 30, and 60 fps;
- first/last position around the GOP boundary;
- invalid 0 and 61 fps, zero hard maximum, and zero generation;
- a new generation after encoder restart;
- actual-class contradiction;
- bounded opaque descriptor projection.

Commands:

```bash
./waf build --targets=unit-tests,UavDroneApp -j2
./build/unit-tests \
  --run_test=UavProtocolState/UavVideoSampleClassScheduleIsBackendTruthfulAtSupportedFps
./build/unit-tests \
  --run_test=UavProtocolState/UavStreamDescriptorProjectsLegacyBoundedOpaqueClass
```

Result: PASS. No generic Core or binding source was changed.
