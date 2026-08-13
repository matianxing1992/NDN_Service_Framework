# Single-AP range/speed matrix and no-failover controls (2026-08-06)

> Historical configuration note: the NDNSF cells summarized below predate the
> SVS `rate_rps` fix and were captured with `expectedRps=0`.  The corrected
> 50 m / 2 m/s three-seed result is now recorded in
> [corrected-primary-50m2ms-3seed-20260806.md](corrected-primary-50m2ms-3seed-20260806.md).
> The old NDNSF values for the other conditions are provisional diagnostics,
> not final evidence.

## Scope and reproducibility

The registered primary matrix uses one physical AP, four Providers
(`ucla,wustl,uiuc,arizona`), `block_network=true`, 300 logical requests per
cell (60 s at 5 RPS), global deadline 5 s, NDNSF ACK timeout 1 s, sequential
gRPC/NSC attempt timeout 1 s, and gRPC application-health routing disabled.
Coverage radii are 50 m and 100 m; fixed drone speeds are 2 m/s and 15 m/s;
seeds are 40, 41, and 42.  Thus the primary campaign contains 36 cells and
10,800 logical requests.  The separate no-failover pilot contains six cells at
50 m/2 m/s (three seeds each for `gRPC-1` and `NSC-1`) and 1,800 requests.

Primary campaign directory:

```text
/tmp/ndnsf-single-ap-primary-matrix-20260806-v3
aggregate.json sha256=c061185be955446d5bd410444235f5f32ce672360f2c58f4d2606083232ce4b4
registration.json sha256=30314be1490fb8e2144cdfce375ab9b7572dac98141f1be6e7cb6586306836ab
```

Control pilot directory:

```text
/tmp/ndnsf-single-provider-pilot-50m-2ms-20260806
aggregate.json sha256=b1cc351ccf3c07901e7f263847c061edfdc1e439062b5aff4cd8a36917f97f7f
registration.json sha256=a8a1f82c8cd4fcfb7542c3b552e93756db396adf7deb4ff274e579237116b6f8
```

Latency is the successful-response end-to-end latency.  `attempts/request`
and `failovers/request` are meaningful for sequential gRPC/NSC.  NDNSF does
not retry another Provider for a request; its comparable field is
`provider-executions/request`, while `failovers/request=0` by design.  Every
cell also retains `p50_ms`, `p95_ms`, and `p99_ms` in `summary.json` and
`aggregate.json`.

## Primary four-Provider results

| range/speed | system | success | attempts/request | failovers/request | provider-executions/request | mean success latency (ms) |
|---|---|---:|---:|---:|---:|---:|
| 50 m / 2 m/s | NDNSF | 72.000% | N/A | 0 | 0.720 | 80.3 |
| 50 m / 2 m/s | gRPC-SEQ-4 | 71.778% | 2.463 | 1.463 | 2.463 | 119.2 |
| 50 m / 2 m/s | NSC-4 | 70.111% | 2.482 | 1.482 | 2.482 | 858.4 |
| 50 m / 15 m/s | NDNSF | 33.667% | N/A | 0 | 0.351 | 1,269.5 |
| 50 m / 15 m/s | gRPC-SEQ-4 | 35.333% | 3.407 | 2.407 | 3.407 | 302.9 |
| 50 m / 15 m/s | NSC-4 | 39.778% | 3.327 | 2.327 | 3.327 | 1,332.6 |
| 100 m / 2 m/s | NDNSF | 99.667% | N/A | 0 | 0.998 | 95.8 |
| 100 m / 2 m/s | gRPC-SEQ-4 | 100.000% | 1.346 | 0.346 | 1.346 | 163.8 |
| 100 m / 2 m/s | NSC-4 | 100.000% | 1.353 | 0.353 | 1.353 | 376.1 |
| 100 m / 15 m/s | NDNSF | 87.778% | N/A | 0 | 0.888 | 599.3 |
| 100 m / 15 m/s | gRPC-SEQ-4 | 83.000% | 2.340 | 1.340 | 2.340 | 469.3 |
| 100 m / 15 m/s | NSC-4 | 83.111% | 2.319 | 1.319 | 2.319 | 1,001.2 |

Across all 36 primary cells (3,600 requests per system): NDNSF achieved
73.278% success, gRPC-SEQ-4 72.528%, and NSC-4 73.250%.  The weighted mean
success latency was 377.6 ms for NDNSF, 257.1 ms for gRPC-SEQ-4, and 798.7 ms
for NSC-4.  gRPC and NSC issued 2.3889 and 2.3703 attempts/request on average,
equivalent to 1.3889 and 1.3703 extra failovers/request; NDNSF issued no
cross-Provider retry and observed 0.7392 Provider executions/request.

The paired three-seed NDNSF-minus-baseline success differences are mixed:

- 50 m / 2 m/s: +0.222 percentage points versus gRPC and +1.889 pp versus NSC.
- 50 m / 15 m/s: -1.667 pp versus gRPC and -6.111 pp versus NSC.
- 100 m / 2 m/s: -0.333 pp versus both baselines.
- 100 m / 15 m/s: +4.778 pp versus gRPC and +4.667 pp versus NSC.

Therefore this matrix does not justify an unconditional “NDNSF is better”
claim.  The defensible result is conditional: NDNSF is favorable in the
100 m/15 m/s handoff condition and competitive at 50 m/2 m/s, while it is not
superior at 50 m/15 m/s or 100 m/2 m/s.  Three seeds are descriptive evidence,
not a general statistical claim.

## Single-Provider no-failover control

At 50 m/2 m/s, the fixed `ucla` controls completed 900 requests each:

| system | success | attempts/request | failovers/request | mean success latency (ms) |
|---|---:|---:|---:|---:|
| gRPC-1 | 29.222% | 1.000 | 0.000 | 17.8 |
| NSC-1 | 29.111% | 1.000 | 0.000 | 29.4 |

This confirms that the control really does not retry another Provider.  It
also shows why the four-Provider comparison matters: at the same trace and
seed set, gRPC-SEQ-4 reached 71.778% and NSC-4 70.111%.  The control is a
diagnostic baseline, not an apples-to-apples replacement for the dynamic NDNSF
selection path, because it intentionally fixes one Provider (`ucla`).

## Verification

- Primary MiniNDN matrix: 36/36 cells passed; control pilot: 6/6 passed.
- Python regression set after the changes: 40 passed, one existing Matplotlib
  deprecation warning.
- `python3 -m py_compile` and `git diff --check` passed.
- `App_WifiMobilityUser` rebuilt with the repository Waf target; the NSC
  consumer was rebuilt with the Boost 1.71/system-library link command.
- No campaign processes remained after completion; `/tmp` had 35 GiB free.
