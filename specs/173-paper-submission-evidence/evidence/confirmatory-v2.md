# Spec 173 Confirmatory Evidence

## Frozen campaign

- Campaign: `results/spec173-paper-submission-confirmatory-v2`
- Status: pass; 16 blocks, 37 cells, no outcome-based retries
- Registration SHA-256: `64fce70880fd0451999d5920b9c7730a9c1b74578ad0a193f356529dcdff5310`
- Toolchain-manifest SHA-256: `a633411bacce596e20ceed585d16a36966a929394d97fa8682da865ee3f1ed7c`
- Campaign-manifest SHA-256: `d30892cc1c18e3883d63c0a18b69e5c19833b35b20794f7cc18d4e5c8e73f535`
- Analysis-manifest SHA-256: `cadd897c69c13a203b8adeee9d2db90cb6a716d2c16679ef11e14889c53e8da9`
- Aggregate SHA-256: `cc6c3ffe76b83fb995b1e3333491ada49573bc6b085754f2267e14a2ee63c19b`
- Exclusions SHA-256: `ad113e1ccf6b6a7c339e69e91235b59a4268bea3fd5671d24d0698aa7da3bdce`
- Normalized-runs SHA-256: `656c8793e68cc4bd6388651197db49ecd8212aea4fc414014caa4006fb372bcf`

The independent unit is one process repetition. Values below are descriptive means plus sample standard deviations over `r1`--`r3`; no significance claim is made.

## Admitted one-Provider baseline

At 10 RPS, every system completed 100% of scheduled requests in all three repetitions.

| System | Completion (%) | Mean successful latency (ms) | p95 successful latency (ms) |
|---|---:|---:|---:|
| gRPC | 100.00 +/- 0.00 | 82.57 +/- 0.15 | 84.14 +/- 0.18 |
| NSC | 100.00 +/- 0.00 | 161.45 +/- 0.28 | 163.74 +/- 0.43 |
| NDNSF | 100.00 +/- 0.00 | 403.93 +/- 0.39 | 409.08 +/- 0.40 |

The NDNSF 100-RPS runs issued fewer than the registered 80% of scheduled requests and are excluded. The NDNSF row above is retained as the frozen-campaign historical result, not the current manuscript Table V value: it was measured before the current epoch-cache/runtime build and is superseded by the matched-current rerun below.

## Matched-current Table V replacement

The manuscript Table V used the same wired MiniNDN topology and workload as the frozen gRPC/NSC baseline (`testbed(loss=0%).conf`, User `memphis`, Provider `ucla`, Controller `arizona`, 10 RPS, 60-s measurement, 10-s warmup, 5-ms service delay, 1-s ACK/attempt timeout, 5-s global deadline, disabled admission control). The corrected current-build NDNSF run used three independent processes for each mode and is recorded in `runtime/table5-matched-fixed-20260813/manifest.json`.

| System | Completion (%) | Mean successful latency (ms) | p95 successful latency (ms) |
|---|---:|---:|---:|
| NDNSF (normal) | 100.00 +/- 0.00 | 177.75 +/- 13.08 | 192.69 +/- 31.54 |
| NDNSF (targeted) | 100.00 +/- 0.00 | 89.86 +/- 0.79 | 91.95 +/- 1.59 |

Normal uses request-scoped ACK/Selection discovery. Targeted addresses the known Provider and uses a bootstrapped token fast path. The targeted repetitions set `NDNSF_TARGETED_TOKEN_BATCH_SIZE=256` with adaptive sizing disabled; a separate trace confirmed three 256-pair stores, zero refill failures, and no fallback requests. Targeted remains a contextual path-cost row, not evidence of multi-Provider discovery. The earlier default-adaptive rows are superseded because their small demand-derived batches caused frequent refills and inflated latency.

## Qualified custom-selection result

At 30 RPS, FirstResponding completed all scheduled requests. The custom queue policy completed 41.00 +/- 0.59%; its successful calls had 275.42 +/- 1.88 ms mean latency, versus 691.58 +/- 21.50 ms for FirstResponding. This is not a latency advantage because latency is conditioned on success.

Every successful custom-policy call selected Provider B, and the remaining scheduled requests had no recorded selection. With the registered one-shot 100 ms ACK collection window, the experiment therefore exposes a collection-window mismatch rather than a fastest-Provider result.

## Admission-control disposition

Admission control remains an optional resource-protection mechanism. In overload cells it restricted request injection, but it did not add Provider capacity. The manuscript reports no exact admission table and does not list admission control as a contribution. Disabled cells failed the offered-load validity threshold and are retained as exclusions.

## Selective-ACK correctness

The registered regression passed. It supports only the statement that a declining Provider remains unselected and does not execute the final response. It does not support population-level success percentages or a performance comparison.

## Removed precision

The old multi-rate one-Provider table, Selective-ACK percentages, admission-control percentages, and physical-node percentages were not retained. The physical-node section is qualitative integration evidence only because its baseline Provider sets were unequal.
