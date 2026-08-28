# Quickstart: Stream Prefetch Retention Recovery

## Focused gate

Build and run the Stream and UAV protocol unit suites. The regression must prove both genuine future satisfaction and evicted-name non-admission.

## Network gate

Run one new 60-second zero-loss MiniNDN UAV GUI cell with exact GStreamer identity and `mapped-live-v1-future-on`. Store it below a unique `results/spec123-*` directory; never reuse Spec 121/122 paths.

The analyzer must reject the run unless it has:

- decoded/displayed activity in every five-second bucket and in the final ten seconds;
- no delivery plateau longer than two seconds;
- no terminal pending-future occupation;
- capture-to-decode p95 <=250 ms and p99 <=500 ms;
- eligible future-hit ratio >=99%;
- Interest work <=2x the frozen rollback reference;
- valid exact identity, security, FEC, and queue/drop accounting.

If the cell fails, preserve it once and return to the focused feedback loop; do not rerun the same identity.
