# Spec 136 Corrected Bidirectional Smoke Diagnostics

## Status

These are short 1/3/2-second smoke runs. They diagnose the harness and locate a
useful comparison rate. They are not the preregistered 10/60/10 formal matrix
and cannot produce the final Spec 136 verdict.

## Harness correction

The original benchmark subscribed both peers to `/spec136/publication`.
Because each peer publishes under that parent, it fetched and validated its own
publications before `onDelivery()` discarded them. This violated the experiment
contract:

```text
A publishes and subscribes to B
B publishes and subscribes to A
```

The corrected benchmark subscribes A to
`/spec136/publication/peer-b` and B to
`/spec136/publication/peer-a`, and rejects any run with a nonzero
`selfDeliveries` count.

Retained diagnostic evidence:

- `results/spec136-rsa-single-worker/smoke-20260723T204126Z`
- binary SHA-256:
  `6cd8cbe4c55acc4edcad76315b0ed5ba6cb179c1a1c229fc5587b5fcf3446969`
- result: both publication Fetchers remained at 10 pending Interests per peer;
  the shared subscription made the result unsuitable for the intended
  cross-peer comparison.

## Corrected 400 pps smoke

Evidence:

- `results/spec136-rsa-single-worker/smoke-20260723T204437Z`
- binary SHA-256:
  `39688867d445cd624186f461e8b164cec12fa37b339c33a2eee57700b248d624`
- library SHA-256:
  `65a84879030221d5dc6f6e20603eb171481293a6bed9f880b3d0e7968427975d`
- both cells: `COMPLETE`
- all four peers: attempted rate within 98%-102%
- all four peers: `selfDeliveries=0`
- RSA Data and Interest signing/validation observed without invalid signatures

| Mode | Attempted pps A/B | Measured delivered | Delivered pps/peer | Delivery ratio | Heartbeat p99 |
|---|---:|---:|---:|---:|---:|
| Face-inline RSA | 400.00 / 400.00 | 0 | 0.00 | 0.0000 | 483.638 ms |
| One-worker RSA | 399.33 / 400.00 | 738 | 123.00 | 0.3078 | 88.875 ms |

This retained run is not positive worker evidence: neither mode sustained the
400 pps target. It predates the unique-delivery gate and the Mapping
subscription de-duplication fix.

## Corrected 800/1000 pps boundary

Evidence:

- `results/spec136-rsa-single-worker/smoke-20260723T204551Z`
- same binary and library hashes as the corrected 400 pps smoke
- all eight peers satisfy attempted-rate admission
- all cells report zero measured delivery
- at 1000 pps both modes still have outstanding publication work at drain end

| Rate/peer | Face-inline delivered | One-worker delivered | Interpretation |
|---:|---:|---:|---|
| 800 | 0 | 0 | common end-to-end collapse in the short window |
| 1000 | 0 | 0 | common end-to-end collapse with remaining work |

These cells do not disprove the 400 pps benefit. Later signer-service
instrumentation shows they overload the serialized RSA path before a stable
Sync/fetch comparison is possible; they must not be labeled as a measured
Sync/fetch ceiling.

## R3 signer and four-core correction

R3 separates signer mutex wait from cryptographic service, enables the same
existing 5 ms Sync batching in both modes, and rejects estimated serial-signer
utilization above 90%.

The retained `smoke-r3-20260723T211329Z` campaign proves the new gate:

- 600 pps passes signer-only utilization admission but exhibits random
  process-level Face starvation on the four-core host;
- 800 pps is rejected at 94.7%-103.7% estimated signer utilization;
- a targeted 16-commit-per-Face-turn probe did not restore 600-pps measured
  delivery and was removed rather than retained as an unsupported Core change.

The sustainable-range campaign is:

- `results/spec136-rsa-single-worker/smoke-r3-sustainable-20260723T212102Z`
- binary SHA-256:
  `23e8c4dadd94ec38bf17a585045ffc547d9db15f8fd3950b27b9edea681250b8`
- library SHA-256:
  `65a84879030221d5dc6f6e20603eb171481293a6bed9f880b3d0e7968427975d`
- all four cells: `COMPLETE`
- all eight peers: attempted-rate, signer-utilization, drain, caller-thread,
  RSA, and self-delivery admission pass

| Rate/peer | Mode | Delivered pps/peer | Delivery ratio | Heartbeat p99 | Delivery p99 |
|---:|---|---:|---:|---:|---:|
| 300 | Face-inline | 298.83 | 0.9972 | 3.878 ms | 671.740 ms |
| 300 | One worker | 299.67 | 1.0000 | 3.290 ms | 58.536 ms |
| 400 | Face-inline | 0.00 | 0.0000 | 412.452 ms | unavailable |
| 400 | One worker | 79.50 | 0.1988 | 139.202 ms | 168.555 ms |

At 300 pps, both modes sustain approximately full delivery and the worker
reduces delivery p99 in this short diagnostic. At 400 pps, both modes are
`LOAD_UNSUSTAINED`; the 0-versus-79.5 pps result is not a valid latency or
capacity comparison. The earlier positive 400-pps interpretation is withdrawn.

## Mapping de-duplication and corrected 400 pps confirmation

Diagnosis found that each repeated Mapping announcement appended the same
subscription to a pending publication. The retained 300-pps diagnostic therefore
reported about 1,200 unique deliveries plus about 4,800 duplicate callbacks per
peer. The regression test
`RepeatedMappingDoesNotDuplicatePendingSubscription` failed `5 != 1` before the
fix and passes after de-duplicating by subscription ID.

The worker boundary was checked independently by
`SingleWorkerPreparesOffFaceAndCommitsOnFaceInOrder`: 128 publications are
prepared outside Face, committed on Face in strict sequence order, and finish
with zero outstanding work.

A common publication Fetch window of 64 then removed the unrelated historical
10-slot receive bottleneck. The 3-second diagnostic
`results/spec136-rsa-single-worker/diagnostic-window64-400-20260723T220100Z`
delivered exactly 400 pps/peer in both modes with zero timeout.

The fresh 10/60/10 confirmation is:

- `results/spec136-rsa-single-worker/confirmation-400-r3-fixed-20260723T220300Z`
- binary SHA-256:
  `e6753ac76b196c8680c0cf3e1d9ffa83208192d75f5c84783b6f8b16976ce3dd`
- library SHA-256:
  `7945f22bcdaaef149f4e3cc2a75a39d124e4dfd54d07027cadcc83b4f5b1308f`
- both peers in both cells attempted exactly 400 pps;
- worker delivered exactly 400 pps/peer with zero Fetch timeout and zero
  outstanding work;
- inline delivered 231.44 pps/peer, accumulated 2,173 measured-window Fetch
  timeouts and 352 pending Face publication calls at measure end.

| Mode | Attempted pps/peer | Delivered pps/peer | Delivery ratio | Heartbeat p99 | Delivery p99 |
|---|---:|---:|---:|---:|---:|
| Face-inline RSA | 400.00 | 231.44 | 0.5786 | 19.921 ms | 156.096 ms |
| One-worker RSA | 400.00 | 400.00 | 1.0000 | 11.252 ms | 83.121 ms |

The immutable inline terminal was recorded as `HARNESS_INVALID` by the runner
version used for this campaign because delivery-ratio failure had not yet been
separated from harness admission. The current analyzer preserves that recorded
status and interprets its only two admission messages as
`LOAD_UNSUSTAINED`; attempted rate, thread identity, RSA, process, and
accounting checks otherwise pass. This provenance correction does not rewrite
the terminal receipt.

This single-pair confirmation provides **non-formal descriptive evidence** of a
capacity extension at 400 pps/peer: the interpreted control outcome is
`LOAD_UNSUSTAINED`, while the worker sustains the full offered load. Heartbeat
p99 falls by 43.5%, and skipped heartbeat ticks fall from 89,903 to 49,747
(44.7%), supporting reduced Face obstruction.

Delivery p99 among delivered publications falls by 46.8%, but this is not an
independent latency contrast: inline delivered only 57.86% while worker
delivered 100%, so the two percentiles describe different survivor sets. The
campaign also records 118 duplicate callbacks in inline and 162 in worker; the
analyzer must expose these counts rather than treating them as zero.

The treatment moves publication construction, encoding, and two Data RSA
signatures off Face. It does not move Sync Interest signing, RSA validation, or
Face-owned commit. It does not claim faster RSA; signer service remains
serialized and similarly utilized in both modes. It also does not satisfy
SC-006 or replace the formal ten-cell matrix.

## Remaining gate

T004b remains open. Do not seal or run the formal
200/250/300/350/400 matrix until the independent tamper check and 1000 pps
no-op pacer preflight pass. Do not relabel any smoke as formal evidence.
