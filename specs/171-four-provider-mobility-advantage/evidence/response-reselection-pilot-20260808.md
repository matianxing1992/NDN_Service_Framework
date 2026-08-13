# Bounded Response reselection pilot (2026-08-08)

## Mechanism

Response retry is an explicit, default-disabled option for ordinary
`FirstResponding` calls. The first successful ACK is still selected
immediately. Later successful ACKs are retained as standby candidates with
their Provider tokens. If the selected Provider has not produced a Response
within the registered attempt timeout, the User publishes one Selection for
the next unattempted candidate. The request ID and 5 s global deadline remain
unchanged. The maximum selection count includes the initial Provider and is
four in the four-Provider experiment.

The mobility harness enables the mechanism only with
`--ndnsf-response-retry`; `--attempt-timeout-ms` supplies the Response-attempt
timeout. Summaries record the option, attempt timeout, maximum attempts,
attempts started, reselections, and Provider executions. The diagnostic fault
mode delays one Provider's Response processing and can delay other Providers'
ACKs; it is not part of a paper comparison.

## Pilot progression

Two 15-second mobility pilots at 100 m and 150 m deliberately delayed
Provider A. Neither produced a request that both selected slow A and retained
a standby ACK. The mechanism therefore made no speculative selection; these
runs established the no-candidate boundary but not recovery.

A 10-second 300 m pilot with a 50 ms standby-ACK delay also produced no
reselection: A's immediate Selection reached the other Providers before their
delayed ACK publication. This confirmed that delaying standby ACKs is the
wrong way to construct the regression.

The final controlled mechanism pilot used 300 m full coverage, seed 61, 5 RPS,
5 ms normal processing, a 3,000 ms Response delay at Provider A, no standby ACK
delay, a 1,000 ms Response-attempt timeout, and the unchanged 5,000 ms global
deadline. It completed 25/25 requests with 26 Provider executions. One request
selected A, retained B's ACK, timed out the A attempt after 1,000 ms, reselected
B, and observed B's Response about 67 ms later. The analyzer reports one
reselection, one success after reselection, and zero timeouts after
reselection.

Evidence root:

- `results/ndnsf-periodic-diagnostic-20260808/t017-response-reselection-mechanism-300m-5s-noackdelay-v3-linked/`

This proves that the bounded state transition works in real MiniNDN and that
retry cost is counted.

## 150 m fixed-trace gate repair

The first 150 m / 10 m/s / seed-61 replay exposed a separate integration bug.
All 75 requests were published while all four Providers were reachable, and
all four Providers published ACKs for each of the four eventual timeout
requests. The User also fetched ACK Data from A, B, C, and D, but matched only
the first ACK. `ServiceUser::OnRequestAck()` discarded every later ACK before
decryption as soon as `FirstResponding` selected its initial Provider. The new
post-decryption standby-candidate logic therefore never saw those ACKs.

The repair preserves immediate FirstResponding selection and changes only the
explicit response-retry path: while a request is still pending, an ACK from a
different Provider may proceed through decryption and token validation so it
can become a standby candidate. Default-disabled FirstResponding, Targeted,
collaboration, and custom-selection paths keep their prior gate.

A strictly paired traced replay used the same mobility trace, 150 m radius,
10 m/s speed, seed 61, 5 RPS for 15 seconds, 500 ms ACK timeout, 1,000 ms
Response-attempt timeout, 5,000 ms global deadline, `block_network=true`,
admission disabled, Provider A delayed by 3,000 ms, and no standby-ACK delay.
It completed 75/75 requests. All 75 requests retained at least one standby
candidate; 17 initial Response attempts timed out, all 17 reselected another
Provider, and all 17 completed successfully. The run recorded 92 Provider
executions for 75 requests, so the recovery work is explicit rather than
hidden.

Paired diagnostic progression:

| Build | Success | Mean | p95 | Candidates | Reselections recovered |
|---|---:|---:|---:|---:|---:|
| Before ACK-gate repair | 71/75 | 470.95 ms | 3,090.66 ms | 34/75 | 5/5 |
| After ACK-gate repair | 75/75 | 315.34 ms | 1,104.06 ms | 75/75 | 17/17 |

Primary traced evidence root:

- `results/ndnsf-periodic-diagnostic-20260808/t017-response-reselection-pilot-150m-15s-late-ack-gate-fix-traced-v3-linked/`

The replay explicitly bound
`NDNSF_MOBILITY_BUILD_DIR=build-new-svs-20260808` and enabled
`NDNSF_MOBILITY_NDN_LOG=ndn_service_framework.*=TRACE`. One earlier launch
fell back to the stale default `build/` tree; the harness rejected it because
the User did not report response retry enabled, and that run is excluded.

## SC-011 independent 60-second replays

Run 1 generated the canonical 60-second seed-61 trace and served only as the
trace-generation/preflight run. Formal SC-011 evidence is Runs 2, 3, and 4:
three fresh MiniNDN processes that explicitly replayed Run 1's trace file.
Every run used 150 m coverage, 10 m/s, 5 RPS, a 500 ms ACK timeout, a 1,000 ms
Response-attempt timeout, the unchanged 5,000 ms global deadline,
`block_network=true`, admission disabled, a 3,000 ms Provider-A Response
delay, no standby-ACK delay, and reconnect face/FIB repair.

| Replay | Success | Attempt-timeout requests | Requests recovered by reselection | Total reselections | Provider executions | Mean | p95 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Run 2 | 300/300 | 58 | 58 | 58 | 358 | 272.56 ms | 1,099.73 ms |
| Run 3 | 300/300 | 60 | 60 | 62 | 360 | 284.45 ms | 1,099.00 ms |
| Run 4 | 300/300 | 59 | 59 | 59 | 359 | 276.90 ms | 1,096.03 ms |
| **Pooled** | **900/900** | **177** | **177** | **179** | **1,077** | **277.97 ms run mean** | **1,096.03–1,099.73 ms** |

All 900 requests were published with at least two Providers reachable. Every
one of the 177 requests with a timed-out Response attempt reselected and
completed; none timed out after reselection, and no bounded-exhaustion case was
needed. Two requests required a second reselection, which explains 179 total
reselections for 177 affected requests. Measurement-barrier lateness was
0.40–0.55 ms.

Formal evidence roots:

- `results/ndnsf-periodic-diagnostic-20260808/sc011-response-reselection-150m-seed61-run2-60s-traced-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/sc011-response-reselection-150m-seed61-run3-60s-traced-v3-linked/`
- `results/ndnsf-periodic-diagnostic-20260808/sc011-response-reselection-150m-seed61-run4-60s-traced-v3-linked/`

SC-011 passes. T017 is complete, and the range/speed comparison matrix may now
resume. These fault-injected runs validate bounded recovery; they are not by
themselves a cross-system mobility-advantage result.

## Verification

- Targeted build with the system linker: passed.
- New real-subscription-callback late-ACK regression: passed.
- Full `SelectionStrategies` suite: 22/22 passed.
- Full C++ unit-test binary: 458/458 passed.
- Lifecycle analyzer suite: 4/4 passed.
- Five Spec-171 Python contract/analyzer suites and `py_compile`: passed.
