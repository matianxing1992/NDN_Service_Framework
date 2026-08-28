# Frozen MiniNDN Non-Regression Campaign

Date: 2026-07-14  
Verdict: **PASS — 20/20 cells, 1,200/1,200 measured requests**

Candidate: `spec111-local-8c7197afab1d-08b7f38b88ec`  
Baseline: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6`  
Result root: `results/spec111-core-app-separation/non-regression-spec111-local-8c7197afab1d-08b7f38b88ec-final`

- Manifest SHA-256: `3f5a195b1ecaf90ae02bf8a6874d151312b8f6754760cfd6525665f193358506`
- Summary SHA-256: `fb6cafaa25ecbe15602e9b385663bac7c5d744dbda64aa82ba3be13cbf6de8ca`
- Journal: 20 immutable start records and 20 terminal PASS records; no
  duplicate/replacement cell and no cleanup survivor.
- Readiness: treatment and baseline each completed the identical 10-warmup +
  60-measured workload with zero fatal protocol-log findings before cell 1.
- Every cell used the frozen topology, order, seed, workload, timeout, logging
  and 100% lifecycle/timeline sampling recipe. No container runtime, OCI/SIF
  build, iTiger access or Slurm submission occurred.
- The runner process predates the derived `formalComparisonEligible` summary
  field. Eligibility is established from the manifest, absence of a diagnostic
  continuation marker, all 20 PASS results and one treatment candidate ID; the
  analysis tool rejects explicit diagnostic or mixed-candidate evidence.

| Pair | Order | Baseline p50/p95 ms/RSS KiB | Treatment p50/p95 ms/RSS KiB | Measured B/T |
|---:|---|---:|---:|---:|
| 1 | B,T | 116.006 / 142.624 / 857436 | 108.010 / 132.215 / 710232 | 60 / 60 |
| 2 | T,B | 120.155 / 142.442 / 859564 | 123.370 / 149.986 / 709224 | 60 / 60 |
| 3 | B,T | 120.939 / 140.898 / 857364 | 123.181 / 143.808 / 709176 | 60 / 60 |
| 4 | T,B | 109.698 / 131.067 / 858936 | 118.775 / 134.639 / 709224 | 60 / 60 |
| 5 | B,T | 104.496 / 143.452 / 858576 | 86.231 / 131.401 / 709408 | 60 / 60 |
| 6 | T,B | 112.588 / 125.166 / 858144 | 112.488 / 139.819 / 710056 | 60 / 60 |
| 7 | B,T | 117.144 / 138.131 / 857640 | 114.889 / 136.083 / 709332 | 60 / 60 |
| 8 | T,B | 120.453 / 138.526 / 858924 | 120.803 / 143.822 / 709012 | 60 / 60 |
| 9 | B,T | 121.400 / 143.254 / 858172 | 104.217 / 128.938 / 710764 | 60 / 60 |
| 10 | T,B | 117.485 / 141.458 / 857628 | 125.895 / 141.471 / 711592 | 60 / 60 |

The earlier failed roots remain immutable diagnostic evidence in
`completion-hold.md` and `role-entrypoint-remediation.md`; they were not
rewritten or promoted. The final root is a distinct, clean, single-candidate
campaign created only after the Controller/collaboration/SVS/hybrid-epoch
defects and their exact failed seeds passed focused diagnosis.
