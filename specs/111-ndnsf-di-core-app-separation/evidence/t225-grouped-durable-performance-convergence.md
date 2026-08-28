# T225 Grouped Durable Commit and Final Candidate Result

Date: 2026-07-15  
Candidate: `spec111-local-0d80d27b786f-51c95c7ec519`  
Verdict: **T225 implementation/measurement complete; candidate rejected by SC-007**

## Correctness-preserving change

`RuntimeJournal.prepare_envelope()` now prepares the encrypted request/result
envelope without publishing a durable reference. `commit_prepared_envelope()`
then writes the protected envelope and its journal reference in one ordered,
authoritative journal transaction. `APPClient.submit()` and result completion
use this grouped transaction. They do not return success before the encrypted
envelope and reference are durable, and restart recovery can resolve the
embedded protected envelope without a synchronous rebuildable spool mirror.
The legacy standalone envelope writer remains for compatibility.

The hot path therefore uses one authoritative fsync for request persistence and
one for result persistence instead of serializing a protected-spool fsync and a
separate journal fsync for each operation. Persistence errors still terminate
the request as failure rather than publishing a false success.

## Focused gates and bounded prediction

- Runtime journal, request handle, cancellation, and APP SDK compatibility:
  **54/54 PASS**.
- Campaign/preflight tests: **15/15 PASS** before the candidate run; the final
  baseline measurement-compatibility closure is covered by the same suite.
- Incremental C++ unit-test build and targeted native Provider cancellation
  regression: **PASS**.
- Local durable-submit microbenchmark, run once after warmup: **300/300 PASS**,
  p50 `2.517494 ms`, p95 `4.491912 ms`, first/last-window drift `4.964054%`.
- Isolated 70-request MiniNDN prediction:
  `results/spec111-core-app-separation/diagnostic-grouped-journal-only-treatment-06`.
  It completed **70/70**, with p50 `114.7895 ms` and p95 `132.7835 ms`.
  Against the frozen readiness baseline, the predicted changes were
  `-0.304%` and `-6.537%`, so both were within the unchanged 5% upper margin.
  `readiness-result.json` SHA-256 is
  `0148ccb8fc2e55a9e6bc8c9260a81f5d26fdbde373ad8da01df95810f3ac461d`.

Earlier diagnostic failures remain preserved:

- `diagnostic-grouped-commit-treatment-05` did not start requests because a
  public key-provider export was missing;
- `diagnostic-grouped-commit-treatment-05-r1` did not start requests because
  the test-only APP state root was not forwarded;
- `diagnostic-grouped-commit-treatment-05-r2` completed 70/70, but its
  synchronous mirror path predicted p50/p95 `+5.85%/+0.81%`, so that design
  was withheld and no candidate was generated from it.

## Candidate identity

The generator was invoked exactly once. No second candidate was generated.

- Candidate document SHA-256:
  `c923c93b14c17297b9e439c96e4ca887468cda0396a346ca8941b1621527842c`.
- Runtime tree:
  `sha256:0d80d27b786fc092ac9c5ed5cf466271f9975a5c3cfb42f587732a0a7279997e`.
- Native extension:
  `sha256:51c95c7ec5190daecd16e2b0054a92f283314fb41b193fa8910c6393eeca0580`.
- OCI, SIF, container build, and iTiger execution identities remain
  `DEFERRED_TO_SPEC110`; Spec 111 invoked no container runtime or Slurm job.

## Baseline compatibility closure

The first formal-readiness directory is preserved at
`campaign-grouped-journal-only-0d80d27b786f-readiness`. Treatment completed
70/70, but a reconstructed pure-old-protocol baseline timed out its first
warmup request. No campaign root or matrix cell was created. Investigation
showed that the old APP must remain the baseline while the fixed-rate pacing,
request identity, authenticated response, and reliable publication wire
changes are shared as measurement-protocol compatibility.

After rebuilding that bounded closure, a one-request baseline smoke passed at
`diagnostic-baseline-protocol-compat-smoke` (`238.77 ms`). Controller,
Provider, and User startup-import preflights passed 3/3 for both roots. The
formal readiness under the final root then passed 70/70 for treatment and
70/70 for baseline.

## Frozen formal campaign

Formal root:
`results/spec111-core-app-separation/campaign-grouped-journal-only-0d80d27b786f-protocolfix`

- All **20/20** cells reached terminal `PASS` exactly once.
- Baseline completed **600/600** measured requests; treatment completed
  **600/600**.
- Failed requests: **0/1200**.
- Throughput: `1.0 request/s` for both variants in every pair.
- Cleanup survivors: **0**; treatment APP state roots were removed.
- Automatic reruns: **0**; diagnostic continuation: none.
- Candidate identity was constant for all ten treatment cells.

Immutable digests:

- campaign manifest:
  `d8dcb46315faefed2478f3ea42e43a1588d1a198ed95703f5ea36f0bc779d865`;
- campaign summary:
  `307803d28f92b96d368e98740d64f14bfa6ceced6dce90234b5d096446b01862`;
- paired analysis:
  `db57cd18ab49513194b4c15d08292eff38b2ccc296bda216ecb6f25587a0b775`;
- treatment readiness:
  `54062a03e5fb99e8435b2a7bf31a2937e290eef96ff68a799379782a177497de`;
- baseline readiness:
  `29c2c3faaa9bad1ddf831487fb420ec4680022b8ccf1cf94bc97d62da9e18a92`.

## SC-007 result

Correctness, completion, throughput, and memory gates passed. Treatment median
process-tree RSS improved by `12.665%`. The latency point estimates were inside
5%, but the predeclared bootstrap interval gates were not:

| Metric | Median paired change | 95% bootstrap interval | 5% gate |
| --- | ---: | ---: | --- |
| p50 | `+3.6245%` | `[-0.2607%, +7.9606%]` | **FAIL** |
| p95 | `+1.9485%` | `[-0.5412%, +21.0150%]` | **FAIL** |
| throughput | `0.0000%` | `[0.0000%, 0.0000%]` | PASS |

Pair 8 was a latency outlier (`+43.7063%/+42.4009%` p50/p95), and pair 9 had
a `+39.5989%` p95 change. They are retained as measured outcomes. Removing
them, weakening the 5% margin, or rerunning the candidate after observing the
result would violate SC-007.

## Closure consequence

T225 has executed its bounded design, tests, prediction, single candidate, and
single clean matrix, so its engineering task is complete with a negative
candidate result. The candidate is **not accepted**. T213 explicitly requires
an accepted candidate; therefore the final audit, Spec 110 handoff, completion
summary, and completion claim remain blocked. The current task list authorizes
no further candidate or matrix. A new, explicitly authorized performance
revision is required before Spec 111 can reach PASS.
