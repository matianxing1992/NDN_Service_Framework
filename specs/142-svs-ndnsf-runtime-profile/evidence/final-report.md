# Spec 142 Final Report

## Validity first

| Cell | Validity | Outcome | Formal comparison eligible |
|---|---|---|---:|
| 01-face-inline-rsa-400 | PROFILE_INVALID | COMPLETE | no |
| 02-worker-rsa-400 | PROFILE_INVALID | COMPLETE | no |

Qualification verdict: **FAIL**. Paired valid rates: none.

The 600/800 pps stage was not authorized and was not started.

## Diagnostic metrics from invalid 400 pps receipts

These rows are boundary diagnostics only. They cannot support a causal worker-performance claim because recovery activated during measurement.

| Mode | Peer | Delivered | Ratio | Mean ms | p50 ms | p95 ms | p99 ms | Pub retry/timeout/Nack | Map retry/timeout/Nack |
|---|---|---:|---:|---:|---:|---:|---:|---|---|
| face-inline-rsa | peer-a | 24000/24000 | 100.000% | 362.665 | 109.076 | 2042.328 | 2666.400 | 17/19/0 | 0/0/0 |
| face-inline-rsa | peer-b | 23698/24000 | 98.742% | 399.790 | 93.742 | 2279.096 | 3294.542 | 38/49/0 | 0/20/0 |
| worker-rsa | peer-a | 24000/24000 | 100.000% | 145.699 | 20.156 | 623.534 | 1633.623 | 56/60/0 | 0/0/0 |
| worker-rsa | peer-b | 24000/24000 | 100.000% | 121.577 | 20.298 | 582.707 | 1509.893 | 47/47/0 | 0/0/0 |

## Piggyback, Fetch, RSA, and resource evidence

| Mode | Peer | Piggy eligible/sent/received/delivered | Fallback | Signed Data bytes mean/max | RSA sign/valid/invalid | Max RSS KiB | Samples SHA-256 |
|---|---|---|---:|---|---|---:|---|
| face-inline-rsa | peer-a | 28000/83997/167469/27772 | 228 | 639.0/639 | 84068/56297/0 | 636296 | `76c6ef63709e1c7ab3952bd69aef4b2d218dd9abe826f7f7c9ad7401cc8f5611` |
| face-inline-rsa | peer-b | 28000/83997/165969/27313 | 385 | 639.0/639 | 84019/56115/0 | 633308 | `e8a7b5d5e88deccfe3dcf04caf15be92e749aba29fbd6a6e06109f32672db315` |
| worker-rsa | peer-a | 28000/83976/167949/27825 | 175 | 639.0/639 | 84001/56344/0 | 629868 | `561f2686653371cdcee5d4a22cad9ed762748c2483d84caa3187bad0ef6986eb` |
| worker-rsa | peer-b | 28000/83973/167949/27796 | 204 | 639.0/639 | 84001/56410/0 | 630176 | `48ae0924f300a83e6384d1bfbde8b308968ef29967f5e90b297fe21364f215a9` |

CPU utilization was not recorded by the frozen r4 peer schema; it is reported as unavailable rather than reconstructed after the fact.

## Root-cause evidence preserved before r4

The first corrected V3 campaign exposed a production-path defect: with parallel Sync production enabled, extra blocks built by a worker were discarded when V3 envelope signing returned to the Face thread. A focused unit test now covers that path, and the full NDN-SVS suite passes 75/75. After the fix, piggyback receive recovered from zero, but nonzero fallback Fetch timeout/retry events remained during the formal 60-second window.

## Claim boundary

This is a two-node, bidirectional NDN-SVS microbenchmark using the current NDNSF-effective V3 profile on a configured zero-loss MiniNDN link. It is not a full NDNSF workflow. The result proves that the prior forced-V2 benchmark was not representative and identifies a remaining recovery boundary; it does not prove a clean causal worker advantage.
