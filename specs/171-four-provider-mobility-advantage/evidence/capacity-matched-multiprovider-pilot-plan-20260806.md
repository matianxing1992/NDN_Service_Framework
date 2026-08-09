# Capacity-matched multi-provider pilot plan (2026-08-06)

## Purpose and claim boundary

The existing mobility pilots answer a coverage-gated availability question.
They do not isolate the reason NDNSF has a multi-provider API: one request can
collect authenticated offers from several Providers and commit one selected
execution, whereas a parallel RPC baseline sends the same logical request to
all four endpoints. This addendum tests that mechanism under bounded provider
capacity. It is a complementary mechanism-level result, not a replacement for
Spec 171 SC-005 and not an RF handoff claim.

The pilot is falsifiable. If NDNSF does not preserve success/latency while
using materially less provider work, the paper will report no demonstrated
capacity-efficiency advantage.

## Registered configuration

The first high-load screen used the values below. It is retained as a
diagnostic, not promoted to a paper result: at 20 RPS and 250 ms both NDNSF
and gRPC-PAR-4 saturated their control/deadline path and neither achieved a
useful absolute success rate.

| Item | Value | Fairness reason |
|---|---|---|
| Systems | NDNSF `first-responding`, gRPC-PAR-4, NSC-4 secondary | same four provider identities and service payload |
| Trace | `random-waypoint`, range 200 m, speed 8 m/s, seeds 20/21/22 | deterministic replay; this range keeps all providers reachable in the preflight coverage check |
| Network gate | `block_network=true` | out-of-coverage behavior is still enforced by the same gate; no client sees the trace |
| Provider capacity | 4 service workers per provider for NDNSF and gRPC | removes the existing 4-vs-32 worker mismatch |
| Service time | 250 ms fixed delay | same application work per successful execution |
| Offered load (screen) | 20 logical requests/s for 60 s (1,200 requests/cell) | stress diagnostic only; not a claim-sized result |
| Deadlines | 1,500 ms logical, 400 ms gRPC attempt, 300 ms NDNSF ACK collection | one common logical deadline; attempt/ACK phases are separately recorded |
| Warmup/barrier | 5 s settle, 4 s absolute measurement barrier, 50 ms lateness gate | identical measured interval and startup exclusion |
| Retries/health | gRPC health routing disabled; no hidden retries; NDNSF normal token/permission path | prevents health oracle or authorization bypass from becoming the treatment |

The 200 m trace is selected before looking at outcomes. A trace-coverage
preflight records the fraction of epochs with 0/1/2/3/4 providers in range;
the campaign is invalid if any seed has an all-four-in-range fraction below
0.95 or any all-unreachable epoch. This check is descriptive only and is not
passed to any client. The deterministic preflight currently measures all-four
fractions of 1.000, 0.991, and 0.980 for seeds 20--22 respectively.

## Primary and secondary endpoints

Primary paired endpoints are logical success rate and p95 response latency at
the fixed offered load. The mechanism endpoint is provider execution work per
logical request, including gRPC server-side duplicate executions after a
client-side winner. Report, per seed and pooled by condition:

- sent, accepted, completed, success, timeout/deadline failures;
- p50/p95/p99 latency and measurement-start lateness;
- client attempts and provider executions per logical request;
- gRPC parallel issued/winner/cancellation counts and exact server duplicate
  execution counts;
- NDNSF per-provider execution counts and the fraction of requests with more
  than one provider execution.

The claim-sized work-efficiency pilot uses the same table except for a
pre-registered offered load of 5 logical requests/s (300 requests/cell). This
keeps the service path below capacity so that a success-rate difference cannot
be explained by overload, while the 250 ms service time makes duplicate work
observable. Its result is accepted only if both conditions hold:

1. NDNSF's seed-level paired lower bound is not worse than gRPC-PAR-4 by more
   than 10 percentage points for success, or its p95 latency is at least 20%
   lower while success remains within 5 percentage points; and
2. NDNSF's median provider executions/request is at most half of gRPC-PAR-4's
   and the difference is present in all three seeds.

Otherwise the outcome is recorded as `NO_DEMONSTRATED_CAPACITY_ADVANTAGE`.
The existing SC-005 verdict remains independent and cannot be upgraded by this
pilot.

The completed three-seed pilot passed the work-efficiency gate but did not
produce a terminal-success or comparable-latency advantage for NDNSF. Therefore
it is sufficient to retain the narrower mechanism result; it does not authorize
an expensive full SC-005 mobility repetition. A future confirmatory repetition
should add independent seeds only if the paper needs stronger evidence for the
work-efficiency claim, while the formal mobility-success conclusion remains
unchanged.

## Pre-registered confirmatory repeat

To test whether the work-efficiency finding is seed-specific, an independent
three-seed repeat is registered before execution: seeds 23, 24, and 25, with
the exact claim-sized configuration above (`random-waypoint`, 200 m, 8 m/s,
250 ms service time, 5 RPS, 300 logical requests/cell, four workers,
`block_network=true`, and gRPC-PAR-4 with health routing disabled). The trace
preflight is fixed at all-four coverage fraction at least 0.95 and no
all-unreachable epoch; its observed fractions are 1.0000, 0.9586, and 1.0000.

The repeat is confirmatory only if all nine cells pass their setup, trace,
capacity, barrier, and gate checks. It will report the same two endpoint gates
and will not be pooled with seeds 20--22 until the per-seed records are
validated. A failure of the work-efficiency gate will narrow the paper claim
back to the already registered negative/neutral mobility result; a pass will
support the mechanism claim with six independent seeds, still without
upgrading SC-005.

## Reviewer-facing controls and falsifiers

- The same byte-identical trace, four provider list, request payload, service
  delay, worker count, deadline, traffic barrier, and source/toolchain apply to
  every system cell.
- gRPC-PAR-4 is the strongest relevant baseline for the fan-out question; its
  four attempts are not hidden or counted as one. NSC-4 is retained only as the
  sequential compatibility reference.
- A gRPC RPC cancellation does not retroactively erase non-preemptible service
  work already started. The server ledger records this explicitly; a future
  cancellation-aware implementation must be a separately labelled baseline.
- Setup failures are terminal evidence, not silently retried. Any missing
  system, trace mismatch, worker mismatch, or barrier miss invalidates the
  campaign.
- The result will not be described as mobility/RF superiority, universal
  availability superiority, or a claim about GPU performance.

## Reproduction and retention

Use the existing paired harness with `--service-workers 4`, retain one campaign
directory per seed, and keep only aggregate JSON/CSV, manifests, summaries,
cell/run tables, trace metadata/hashes, and the protocol README. Raw logs and
replay CSVs stay in the temporary campaign root. Run the focused Python suite,
`py_compile`, `git diff --check`, the disk preflight, and a no-process check
before closing the evidence.
