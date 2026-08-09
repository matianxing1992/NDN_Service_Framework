# NDNSF 100 m post-partition recovery diagnosis

Date: 2026-08-08  
Condition: one AP, 100 m coverage, 2 m/s, four Providers, 5 RPS, 300 s
campaign, 1 s ACK/attempt timeout, 5 s global deadline, admission control
disabled, FirstResponding.

## Finding

The low 100 m result is a real NDNSF recovery failure, but it is not an
ACK-to-Response failure and FirstResponding is not missing.

The original campaign summary reports 1500 requests, 329 successes, and 1171
timeouts (21.93%). The 329 requests with Provider Selection are also the 329
Provider executions and 329 final Responses. Thus, once NDNSF receives a
successful ACK and selects a Provider, this run delivers the Response.

An instrumented replay of the same seed-40 trace through its first 130 s
reports:

| lifecycle event | count |
| --- | ---: |
| requests sent | 650 |
| ACK received | 392 |
| ACK matched to pending call | 331 |
| Provider Selection | 331 |
| Provider execution / Response | 331 |
| deadline failures | 319 |

The 319 failed requests have `ackCount=0`, no selected Provider, and no matched
ACK at timeout. Provider logs independently show 331 request/ACK/selection/
Response executions and no new Provider requests after the initial burst.

The replayed mobility trace has one all-unreachable interval from approximately
70.1 s to 101.0 s (30.9 s, 10.3% of the 300 s measurement window). NDNSF stops
receiving new requests at approximately 66 s and does not resume after
coverage returns. A prefix replay ending before that outage succeeds 300/300.

## Interpretation

The failure seam is the NDNSF/SVS request-delivery recovery path after a
temporary partition. A coverage oracle of 86.6--89.7% is only a time-fraction
summary; it hides the fact that one long outage can leave the NDNSF publication
stream wedged for the remainder of the campaign. The 150 m result does not
disprove this: its trace did not exercise the same unrecoverable partition.

Current evidence does not yet identify whether the stuck stage is SVS state
vector advertisement, mapping retrieval, or publication fetch/expiry. The
current configuration uses bounded publication recovery (`retries=2`, inner
retries `=2`, 500 ms lifetime, 50--2000 ms backoff, window 32) and a 30 s
fallback publication-fetch deadline; application `onMissingData` callbacks are
currently no-ops. These are the next instrumentation points, not yet a proven
root-cause fix.

## Next diagnostic step

Add temporary SVS state-vector/mapping/publication-fetch counters and an
`onMissingData` trace, then repeat the original-trace 130 s replay. Only after
locating the stuck stage should we change retry/deadline parameters or add
reselection/recovery logic; do not reinterpret this result as a
FirstResponding-selection bug.
