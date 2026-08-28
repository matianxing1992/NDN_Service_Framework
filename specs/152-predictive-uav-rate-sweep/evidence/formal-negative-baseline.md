# Spec 152 Formal Negative Baseline

Result:
`results/spec152-predictive-uav-rate-formal-20260726T074501Z`

```text
status=FAIL
accepted=4/6
rerunAllowed=false
```

All configured rates were achieved within 0.06%, delivery was
99.864%–99.942%, Mapping Interests were zero, and ready/gap queues ended at
zero. Failures:

- 10 fps: p99 1126.999 ms and launcher return code 1;
- 30 fps: launcher return code 1.

The launcher failures were missing asynchronous Drone `video stopped` markers
at the instant the Ground Station exited. Passing cells show the marker arrives
about one second after the provider final status. The launcher already has a
bounded `wait_log` helper but used immediate `require_log` here.

The 10-fps p50 of 398.483 ms and the inverse period relationship across rates
show that capture-origin was recorded at the unsynchronized raw-frame probe
before the encoded sink waited for PTS. The clock must pace at the capture
identity boundary, with the encoded appsink again delivering immediately.

No cell is replaced or rerun. Spec 153 owns the repair and full successor
matrix.
