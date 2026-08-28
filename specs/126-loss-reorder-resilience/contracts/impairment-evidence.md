# Frozen Impairment Evidence Contract

## Matrix

| Cell | Repetitions | Loss | Netem reorder profile |
|---|---:|---:|---|
| zero-loss | 1 | 0% | none |
| loss | 5 | 1% iid | none |
| reorder | 5 | 0% | delay 20 ms 10 ms normal; reorder 25% 50%; gap 5 |
| combined | 5 | 1% iid | delay 20 ms 10 ms normal; reorder 25% 50%; gap 5 |

Common workload: 60-second measured video, 1000-ms start delay, 1200 kbit/s,
320-pixel width, one repair, adaptive-sample-atomic policy, GStreamer synthetic
source, WARN NFD logging, sampled timelines, one Provider and one consumer.

## Preflight

- exactly one launcher/cleanup owner;
- sufficient disk and noninteractive sudo;
- build/security/deterministic gates pass;
- source hash and Spec 125 evidence hash manifest written;
- 16 unique command/run IDs, none previously invoked;
- effective qdisc captured on both endpoints before application start.

## Admissibility

A run is admissible when its command was invoked once and its return/process,
effective impairment, duration, and required metrics are recorded. A failed
return code, app crash, timeout, or failed gate remains admissible negative
evidence. Missing invocation identity or concurrent campaign ownership makes a
run invalid; it is preserved but not silently replaced.

## Freeze

After the first command starts: no workload, threshold, logging, impairment,
repetition, parser, or source change. A source change ends the series and any
new validation uses a complete separately named confirmation series.
