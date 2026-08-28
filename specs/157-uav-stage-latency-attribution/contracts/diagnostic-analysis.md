# Diagnostic Analysis Contract

## Input

- `--campaign-dir`: immutable Spec 156 six-cell result root.
- `--output-dir`: new directory that MUST NOT be equal to or nested below the
  campaign root.
- Each `fps-*` cell supplies `drone.log`, `ground-station.log`, and
  `cell-summary.json`.

## Exact join key

```text
(stream_id, session_epoch, source_frame_id)
```

Provider cursor events add:

```text
(stream_id, session_epoch, publication_cursor) -> source_frame_id
```

The materialization boundary is the `signed-and-materialized` time of the
greatest source cursor assigned to that frame.

## Stage metrics

```text
capture_to_encoded_ms
encoded_to_materialized_ms
materialized_to_decoder_input_ms
decoder_input_to_output_ms
capture_to_output_ms
```

Each metric reports `count`, `mean`, `p50`, `p95`, and `p99`.
The report also includes source segments per frame so Provider materialization
time is interpreted against the amount of per-frame work.

## Exclusion rules

A frame is excluded from stage statistics when any boundary is missing, the
exact frame binding is absent, its decoder output is outside the original
measurement window, or timestamps are not monotonic. Exclusion counts and
reasons remain visible.

## Immutability

The analyzer opens campaign inputs read-only and rejects an output directory
inside the campaign root. It does not invoke MiniNDN or any campaign runner.
