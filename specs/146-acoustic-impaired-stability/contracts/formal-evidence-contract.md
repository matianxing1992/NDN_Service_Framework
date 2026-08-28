# Formal Evidence Contract

## Immutable predecessor

```text
results/spec144-uav-sensor-stream-20260724T165132Z
campaign-summary.json
  01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738
campaign-manifest.json
  5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517
campaign-cells.csv
  c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96
```

Spec 146 MUST NOT write under that result root or invoke a Spec 144 formal
destination. These hashes are checked before implementation and after closure.

## New formal matrix

| Profile | Repetitions | Measured window |
|---|---:|---:|
| zero-loss | 1 | 60 s |
| loss | 5 | 60 s |
| reorder | 5 | 60 s |
| combined | 5 | 60 s |

Exactly 16 unique terminal rows are required. A crash, timeout, count mismatch,
readiness failure, analyzer failure, or incomplete cell remains evidence and
cannot be replaced.

## Frozen gates

- delivery at least 99.9%;
- mean, p50, p95, p99, and max reported in every cell;
- zero-loss p95 at most 150 ms, p99 at most 250 ms, and longest complete-block
  gap at most 160 ms;
- impaired-profile p95 at most 250 ms, p99 at most 400 ms, and longest
  complete-block gap at most 320 ms;
- provider-confirmed future-hit at least 95% when defined;
- Mapping novelty at least 99%;
- nonproductive Payload Interest ratio at most 10%;
- exact conservation for Mapping/Payload, retry, terminal outcome, repair
  consumption, and recovered provenance counts.

No gate may change after the first formal cell begins.

## Measurement instrumentation

Every provider, consumer, and controller command MUST contain:

```text
NDNSF_STREAM_PACKET_TIMELINE_TRACE=0
```

The high-rate packet trace may be enabled only in a separate non-formal
diagnostic destination. This removes measurement instrumentation, not protocol
work: Stream window, FEC, retry, Mapping, and network-profile values remain
unchanged.

## Frozen subject

The one-shot manifest hashes the Core library, controller and acoustic-node
binaries, Python binding, all Stream/validator source files that determine
those binaries, the wrapper/runner/analyzers, application fixture/configuration,
and all Spec 146 requirements/contracts. Mutation after the first formal cell
is a campaign failure.

## Recovery units

Report separately:

- recovery-eligible source count (post-timeout or terminal);
- terminal missing source count;
- recovered source count;
- recoverable group count;
- recovered group count;
- algorithm invocation count;
- recovery exhaustion count.

Source recovery ratio divides recovered sources by recovery-eligible sources;
group recovery ratio divides recovered groups by recoverable groups. No
percentage may divide a source count by an invocation count.
