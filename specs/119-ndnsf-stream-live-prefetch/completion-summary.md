# Spec 119 Completion Summary

## Outcome

NDNSF now exposes one app-neutral C++/Python LiveStream API over meaningful
application Data names:

```text
ServiceProvider::createLiveStream(definition)
ServiceUser::openLiveStream(descriptor, options)
```

The Provider reserves original names before production and publishes signed,
predictable Mapping blocks. The Consumer resolves cursor to original name and
expresses exact Interests for that name. Payload Content remains opaque; Core
contains no encryption key, cipher, encrypt or decrypt API. Optional bounded
XOR FEC operates over opaque bytes and reports recovered provenance without
caching or republishing reconstructed content as original Data.

## Public API And State Closure

- `LiveStreamPublisher` owns Mapping/payload route registration, immutable
  reservation, materialize-once publication, atomic activation, retention and
  bounded Provider pending state.
- Mapping and payload future Interests have independent caps, lazy expiry and
  near-cursor priority. Unmapped, malformed and too-far names allocate no
  Provider pending state.
- `LiveStreamConsumerHandle` owns Mapping validation, exact-name scheduling,
  retry/timeout/Nack accounting, aggregate in-flight limits, optional local FEC
  recovery, status and idempotent stop.
- `mapped-pressure`, `mapped-live-v1-future-on` and
  `mapped-live-v1-future-off` share the same descriptor and wire identity.
  Measured evidence selected `mapped-pressure` as the default.

## Deterministic And App-Neutral Evidence

- Full C++ suite: 309/309 passed; focused `Stream`: 33/33 passed.
- Python Core streaming: 18/18 passed.
- Campaign/parser tests: UAV 8/8 and prefetch 3/3 passed.
- C++ `LiveStreamProvider` and `LiveStreamConsumer` examples compiled.
- 0% dual-consumer MiniNDN:
  `results/spec119-live-stream-minindn-dual-20260718-candidate2` passed;
  Beginning received 12/12 and Latest received six items from its refreshed
  safe-join descriptor.
- 5% FEC-enabled MiniNDN:
  `results/spec119-live-stream-minindn-fec-loss05-20260718-candidate4` passed
  with 12/12 source items and only original semantic names.

The deterministic suite covers each one-source-loss XOR position plus corrupt,
wrong-digest, wrong-length, wrong-group, two-loss, expired and oversize cases.
The network run did not manufacture packet loss to force a recovery counter;
zero network recovery is not presented as an FEC improvement.

## Frozen Matched Campaign

Evidence root:

```text
results/spec119-live-prefetch-acceptance-20260718-candidate1
```

The orchestrator completed 30/30 accepted fresh-process cells: three policies,
0%/5% loss, five matched 60-second repetitions, counterbalanced with seed
`11920260718`. Source and runtime-binary digests, commands, policy order and
per-run evidence are retained. No failed cell was automatically retried.

Median run metrics:

| Loss | Policy | p95 capture-to-decode | Timeout/Nack count | Mapping bytes | Mapping Interest share | Max Core in-flight |
|---:|---|---:|---:|---:|---:|---:|
| 0% | mapped-pressure | 258.05 ms | 0 | 14,702 | 7.14% | 9 |
| 0% | future-on | 255.00 ms | 0 | 14,702 | 7.41% | 5 |
| 0% | future-off | 255.00 ms | 0 | 12,246 | 6.67% | 1 |
| 5% | mapped-pressure | 43,286.85 ms | 10 | 9,788 | 7.04% | 9 |
| 5% | future-on | 25,238.00 ms | 22 | 42,805,592 | 98.85% | 66 |
| 5% | future-off | 40,773.10 ms | 8 | 9,788 | 7.58% | 1 |

Every future-on cell met the nonzero-denominator and at-least-99% Provider
Interest-before-production hit gate. The adoption gate nevertheless failed:

- 0%: 3/5 favorable pairs; median best improvement 0.39%.
- 5%: 0/5 favorable pairs; median lag improved 44.0%, but timeout/Nack load
  worsened 137.5%.

Therefore future-on is retained as an explicit experimental policy, not the
default. This is a negative performance result, not a completion failure.

## Final-Code Protected Validation And Limits

`results/spec119-post-hardening-acceptance-20260718` reran one fresh protected
60-second UAV cell at 0% and 5% after pending-table hardening. Both passed
video, control, security and bounded-state gates. The 5% cell still produced
1,030 Mapping Interests and 2,527,190 Mapping bytes (93.6% Interest share), so
loss-driven Mapping retry amplification remains the most important efficiency
follow-up. It does not invalidate bounded correctness, but it prevents any
claim that Mapping overhead is already optimized.

MiniNDN proves the implemented protocol path, not real Wi-Fi, camera hardware,
container/iTiger deployment or long-duration operation. Those remain separate
deployment experiments.
