# Spec 143 Final Report

## Outcome

Spec 143 is closed as **DIAGNOSED**. The exactly-once worker diagnostic
reproduced the zero-configured-loss Fetch timeout boundary and classified all
829 measurement-window timeouts. It did not rerun or modify Spec 142, execute
the conditional inline cell, tune NDN-SVS, or implement a recovery change.

Canonical campaign:

`results/spec143-svs-zero-loss-fetch-causality/diagnostic-20260724T020332Z`

## Frozen Experiment

| Item | Value |
|---|---|
| Nodes and traffic | Two MiniNDN nodes; both publish and subscribe |
| Link | 100 Mbps, 10 ms one-way delay, 0% configured loss |
| Runtime | NDNSF V3 profile; 800-byte piggyback; Fetch window 128 |
| Security | RSA-2048 Data signing and validation |
| Publication path | One publication-preparation worker |
| Offered load | 400 pps per peer |
| Window | 10 s warmup, 60 s measurement, 10 s drain |
| CPU affinity | CPUs 0–3 |
| Formal cells | One: `01-worker-rsa-400` |

Both peers scheduled, attempted, and accepted exactly 24,000 publications in
the measurement window: 400.0 pps per peer and 0% deviation from target.

## Fetch Counters

All values below are measurement-window deltas.

| Peer | Publication timeout | Inner retry | Outer retry activation | Mapping timeout | Publication/Mapping Nack |
|---|---:|---:|---:|---:|---:|
| peer-a | 576 | 394 | 18 | 116 | 0 / 0 |
| peer-b | 116 | 76 | 2 | 21 | 0 / 0 |
| Total | 692 | 470 | 20 | 137 | 0 / 0 |

The 692 publication and 137 Mapping timeouts equal the 829 classified
consumer timeout attempts.

## Causal Result

| Semantic kind | No exact-Nonce producer observation | Producer put without consumer Data | Total |
|---|---:|---:|---:|
| Publication | 478 | 214 | 692 |
| Mapping | 77 | 60 | 137 |
| Total | 555 | 274 | 829 |

Measured conclusions:

- 555/829 timeouts (66.9%) had no producer-application event for the exact
  Interest name and nonce.
- 274/829 (33.1%) reached the producer application and caused a Data put, but
  the consumer had no Data callback for that attempt before timeout.
- No measurement-window timeout was caused by a real publication store miss
  or an empty MappingProvider response.
- Among the 555 exact-Nonce misses, 253 had the same name observed at the
  producer under another nonce. This is consistent with retry overlap or NFD
  PIT aggregation, but it does not prove either mechanism. The remaining 302
  had no same-name producer observation.

Therefore, the observed boundary is not publication encoding, signing, or
producer DataStore lookup. Attempts stop between consumer dispatch and
producer callback, or after producer Data put but before the consumer Data
callback. Distinguishing PIT aggregation from forwarding/transport queue loss
requires NFD-level evidence in a new Spec.

## Resource And Perturbation Boundary

| Peer | Process CPU, one-core normalized | Four-core normalized | System CPU in 60 s | Threads | Max RSS | Trace size |
|---|---:|---:|---:|---:|---:|---:|
| peer-a | 122.42% | 30.60% | 55.784 s | 12 | 624,456 KiB | 129,071,302 B |
| peer-b | 107.05% | 26.76% | 49.047 s | 12 | 546,056 KiB | 107,884,737 B |

TRACE generated 236,956,039 bytes and heavily increased system CPU and
latency. These values are diagnostic overhead, not a new NDN-SVS performance
result. Spec 142 had already observed the timeout/retry boundary with WARN-only
logging, so TRACE amplified the workload but did not originate the research
question.

## Analyzer Revision

The first offline analysis incorrectly gave generic `producer_store_miss`
precedence for Mapping Interests. Mapping Interests overlap the general
SVSync DataStore prefix, so that miss is expected when MappingProvider handles
and answers the same Interest. The analyzer was corrected, tested, and rerun
only on the retained raw traces.

- Network cell rerun: false
- Frozen analyzer SHA-256:
  `7c5babae600f7b285b7b4e467fa55d18d61cc5c5c24cb9d812ba56e323273843`
- Corrected analyzer SHA-256:
  `a7c94dec9589c597a17d1d27f809950efff151e7a1384d38f47377131535a1f3`
- Corrected classification summary SHA-256:
  `1d82535bffafb5155260d554d5d20872cb0b6f3284c6ccaf00ef824593afc4a0`

The authoritative revision receipt is
`01-worker-rsa-400/analysis-revision.json`.

The original all-source build verifier now intentionally rejects the working
tree because the offline analyzer SHA-256 differs from its pre-cell frozen
value. Closure therefore verifies two linked identities: the binary, library,
benchmark, runner, builder, and five NDN-SVS runtime sources still match the
frozen manifest; the revised analyzer and revised summary match the
post-cell revision receipt. Treating the original all-source verifier as PASS
after this correction would be incorrect.

## Verification

- Spec 143 focused Python tests: 20/20
- Spec 142 compatibility tests: 11/11
- NDN-SVS unit tests from the correct repository working directory: 75/75
- NDNSF unit tests: 363/363
- Pre-cell build and runtime manifests: PASS
- Post-cell dual identity check: PASS (frozen runtime subject plus revision
  receipt); the original all-source verifier correctly reports the analyzer
  change
- Spec 142 baseline tree hash unchanged:
  `12c7d504db09acaaa16241ea7fa410e7c50cac0cbb54c237852f856c8848cb3d`

## Claim Boundary

This is one causal diagnostic cell. It does not estimate run-to-run variance,
prove physical packet loss, prove PIT aggregation, compare inline and worker
performance, or validate a recovery fix. Zero configured link loss does not
mean that application, NFD, kernel, or transport queues cannot discard or
delay work.
