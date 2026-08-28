# Spec 135 RSA-2048 NDN-SVS Queue Causality Report

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: validate
- Verification Status: MEASURED
- Version Label: code_result_v1

## RSA boundary result

Selection: **first-rsa-instability-boundary** at **400 pps/peer**. Every peer proved TLV signature type 1 (`SignatureSha256WithRsa`) before warmup; RSA key generation and the probe were outside the measured window.

| Rate | Mean attempted/scheduled | Aggregate delivered/attempted | RSA inner+outer sign (us) | PUB.TOTAL (us) | Payload queue (us) |
|---:|---:|---:|---:|---:|---:|
| 200 | 0.9923 | 1.0000 | 1099.657 | 1160.512 | 10.095 |
| 400 | 0.9732 | 1.0000 | 1093.112 | 1148.612 | 6890.290 |
| 600 | 0.8197 | 1.0000 | 1102.879 | 1158.414 | 25087.372 |
| 800 | 0.5396 | 1.0000 | 1099.327 | 1155.669 | 34567.207 |
| 1000 | 0.3145 | 0.0598 | 1089.105 | 1142.143 | 9446320.626 |

The preregistered 98% operational boundary is 400 pps/peer. The more pronounced capacity knee is 600 pps: attempted/scheduled falls to about 82%; at 800 it is about 54%. At 1000 pps the complete publication call already exceeds the 1 ms release period before receive/fetch callbacks are counted, and aggregate delivery collapses to about 6%.

## RSA cost versus the frozen DigestSha256 baseline

| Rate | Digest inner+outer sign (us) | RSA inner+outer sign (us) | RSA/Digest | RSA share of PUB.TOTAL |
|---:|---:|---:|---:|---:|
| 200 | 26.61 | 1099.66 | 41.3x | 94.8% |
| 400 | 20.33 | 1093.11 | 53.8x | 95.2% |
| 600 | 19.84 | 1102.88 | 55.6x | 95.2% |
| 800 | 23.15 | 1099.33 | 47.5x | 95.1% |
| 1000 | 22.08 | 1089.11 | 49.3x | 95.4% |

The Digest values are the already frozen Spec 133 combined-peer means; they are not remeasured or relabeled. RSA-2048 consumes about 1.09--1.10 ms for the two Data signatures per `publish()` and roughly 95% of `PUB.TOTAL`. Thus the earlier 14--17 us inner-sign values were correct for DigestSha256 but cannot represent the deployed RSA path.

## Publication stage detail

Combined-peer exact mean per completed call:

| Stage | 200 | 400 | 600 | 800 | 1000 |
|---|---:|---:|---:|---:|---:|
| Inner Data build (us) | 1.706 | 1.331 | 1.219 | 1.257 | 1.340 |
| Inner RSA sign (us) | 568.496 | 562.556 | 565.927 | 558.726 | 554.036 |
| Outer Data build (us) | 2.396 | 2.206 | 2.178 | 2.272 | 2.278 |
| Outer RSA sign (us) | 531.161 | 530.556 | 536.952 | 540.601 | 535.070 |
| Outer store insert (us) | 23.484 | 20.946 | 21.047 | 21.340 | 19.936 |
| Outer Face put (us) | 4.768 | 4.347 | 4.371 | 4.472 | 4.275 |
| Complete publish (us) | 1160.512 | 1148.612 | 1158.414 | 1155.669 | 1142.143 |

Encoding/build/store/Face-put work remains in the low tens of microseconds. The approximately 0.56 ms inner RSA sign plus 0.53 ms outer RSA sign dominates the synchronous call.

## Receive/fetch pressure

| Rate | Sync receive mean (us) | Payload queue mean | Mapping queue mean | Mean payload fallbacks/peer | Missing-batch p95 |
|---:|---:|---:|---:|---:|---:|
| 200 | 105.298 | 0.010 ms | 0.000 ms | 1.0 | 2.0 |
| 400 | 391.605 | 6.890 ms | 0.004 ms | 24145.0 | 17.5 |
| 600 | 668.323 | 25.087 ms | 0.004 ms | 42890.0 | 28.5 |
| 800 | 853.852 | 34.567 ms | 0.003 ms | 40812.0 | 36.5 |
| 1000 | 39996.315 | 9446.321 ms | 9332.399 ms | 14791.5 | 11334.0 |

At 1000 pps, completed Payload and Mapping queue waits are about 9.45 s and 9.33 s, while missing-range batches reach thousands of items. This preserves the later fallback-queue instability found with DigestSha256, but it is no longer the first boundary: RSA signing and shared-loop scheduling reduce attempted load earlier.

## Factor verdicts

- H1 Fetcher window: **PARTIAL**.
- H2 piggyback capacity: **PARTIAL**.
- H3 shared I/O thread: **MECHANISM_ONLY_NOT_ISOLATED**; temporal co-variation is measurable, but no unsafe cross-thread treatment was run.

Detailed peer metrics and exact 81-stage totals are in `peer-causality-summary.csv` and `stage-causality-summary.csv`; matched factor deltas are in `factor-contrasts.csv`.

| Contrast | Attempted ratio delta | Missed-release delta | Payload fallback delta | Payload queue delta (us) |
|---|---:|---:|---:|---:|
| window@4096 | -0.00858 | +206.0 | -1313.0 | -6735.4 |
| window@7168 | +0.01150 | -276.0 | -152.0 | -8265.6 |
| piggyback@W10 | -0.00631 | +151.5 | -8217.0 | +1577.4 |
| piggyback@W40 | +0.01377 | -330.5 | -7056.0 | +47.1 |

## Solution directions (analysis only)

1. Treat RSA publication signing as fixed service demand on the shared I/O loop. Cache only safe encoding/key lookup work, batch signatures only if protocol semantics allow it, or move signing through an explicitly thread-safe ordered completion path in a new Spec.
2. Bound missing-range expansion and admit fetch work incrementally, so the later overload regime cannot enqueue one enormous burst.
3. Replace the fixed Fetcher window with a bounded adaptive window only after separating the RSA pacing limit; the matched intervention proves queue reduction but not a consistent attempted-rate improvement.
4. Larger piggyback capacity reliably cuts fallback Interest count in this round, but its queue/pacing effects are mixed; evaluate packet size and fragmentation before adopting it.

No production NDN-SVS or NDNSF change is made by Spec 135.

## Limitations

- One once-only cell per configuration supports descriptive mechanism evidence, not variance estimates or population inference.
- RSA uses ndn-cxx software RSA-2048 with each MiniNDN node's separate persistent file TPM. Hardware TPM/HSM latency is not represented.
- Validators remain disabled to preserve the Spec 133 receive-side control; RSA verification cost is therefore not included.
- The 98% rule is an operational pacing boundary, not CPU saturation. The sharper throughput knee occurs at 600 pps/peer.
- Stage-B interactions are real: neither a larger window nor larger piggyback capacity is an unconditional standalone improvement.

## Fallacy scan

- No cherry-picking: all eight once-only receipts are retained.
- No security relabeling: Spec 133 remains DigestSha256; Spec 135 proves RSA.
- No pseudo-replication: two peers in one cell are paired observations, not independent campaign replicates.
- No p-values or confidence intervals from n=1 cells.
- No sum of queue/network wait with leaf CPU demand.
- No survivor-only queue mean without calls, delivery, and missed releases.
- No throughput claim from offered rate alone.
- No causal shared-thread claim without a thread treatment.
- No automatic retry or replacement of failures.
- No claim that a larger packet is free of fragmentation effects.
- No production-fix claim from a diagnostic-only patch.
