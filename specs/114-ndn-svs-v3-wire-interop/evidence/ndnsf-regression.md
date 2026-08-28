# NDNSF consumer regression evidence

## Spec 115 rewritten-history candidate

The installed and candidate libraries match SHA-256
`615f94f3577ae509f8f98b83527ce8f43c7c0777d70a99d85ddd45d90583ccee`.
NDNSF rebuilt successfully (334 tasks). The C++ Targeted suite passed 18/18
cases and 124/124 assertions. Six native-unittest Python files ran 26 tests:
23 passed and three exclusive MiniNDN cases were skipped by their declared
guards; compiled binding presence and removal of public token-disable controls
passed. The unavailable `pytest` module was not installed solely for this gate.

Date: 2026-07-16  
Status: PASS

## Bound rebuild

- NDN-SVS: `Experimental@53dd1588201b967a4aa9decd3e51ade3263e0f88`
- installed/workspace NDN-SVS library:
  `9988a7e35523cd86dd5feb253078e839a4f8895f7693aeada2e7b3475a70ab92`
- NDNSF shared library:
  `bde4b203ebea0b3b074fdbcb25f9245138d73a0582cf51d1c23ce214c8debd5c`
- NDNSF unit runner:
  `f78a7cd372b4695185113ee5d1065347d838717b3e97e7bcef61f4a21a053c90`
- Python extension:
  `c065fa49a38e206420bcbf4b3025aff60be12270c911800489a942e9e09c2c15`
- consumer candidate: `spec112-7f67052175cf629158ab`
- manifest SHA-256:
  `f1b9e09d248f5c5cbb270a1425944179515b91db838083842d262459c4285976`
- campaign summary SHA-256:
  `d4989fbefeaf5597c4af1a6e21c2a54998ed213d6b7d59542d33cb7740072b3b`

The consumer manifest now declares the exact eight executed cells, including
the async timeout and V2 rollback cells, and binds the actual 1000 ms degraded
timeout rather than the previous stale 4000 ms declaration.

## MiniNDN cells

| Cell | Outcome | Evidence |
|---|---|---|
| boundary-async-normal | PASS | 6/6, 29.184 s |
| boundary-async-targeted | PASS | 6/6, 27.867 s |
| boundary-sync-normal | PASS | 6/6, 28.737 s |
| boundary-sync-targeted | PASS | 6/6, 28.429 s |
| burst-async-normal | PASS | 102/102, 46.353 s, provider alive |
| targeted-degraded-timeout | PASS | bootstrap + timeout at 1001.967 ms |
| targeted-degraded-timeout-async | PASS | bootstrap + timeout at 1051.283 ms |
| rollback-v2-boundary | PASS | 2/2, 27.099 s |

Both degraded cases have exactly one timeout terminal, zero response terminals,
total terminal count one, and `deadlineWithinLimit=true`. They intentionally
record one failed business request after a successful bootstrap; this is the
expected fault outcome, not a regression failure. V2 logs contain exactly the
explicit `version=2 lifetimeMs=1` profile for both User and Provider.

## Focused tests

- NDN-SVS profile default/override: 1/1.
- C++ Targeted: 18/18; NdnSvsSmoke: 2/2; MessageValidator: 2/2.
- Python focused files: 41 passed, 3 MiniNDN-exclusive skips.
- GUI/profile round trip: 16/16.
- invalid protocol startup: rc=2 before request handling.

The skipped Python network paths are covered by the accepted immutable cells.
No container, iTiger, physical-network, or GPU claim is made.

## Preserved earlier evidence

The pre-audit successful candidate `spec112-74bb377bf50383a06657` and its first
trust-policy diagnostic predecessor remain unchanged. Two post-audit manifest
drafts (`spec112-fa09352feab619eac211` and
`spec112-fe990e7595bf46dc068c`) contain no campaign summary because their
declarations were corrected before execution; neither was relabeled or reused.
