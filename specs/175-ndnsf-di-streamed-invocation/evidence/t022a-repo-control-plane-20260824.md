# T022-A repository control-plane checkpoint — 2026-08-24

This checkpoint separates repository artifact preparation from the streamed
M01 subject. Repository publication is a prerequisite for the real
four-Provider run, but its ACK collection is not the streamed invocation
ACK/deadline contract.

## Finding

Run `t022a-repro-20260824f` stopped before M01 because the repository publisher
reported `repo-store-insufficient-cover` with `candidateCount=0`. The
repository Provider had received the STORE Request and logged
`REPO_ARTIFACT_ACK_ACCEPT`, while the User later logged the ACK publication
through SVS. The ACK was not present in the immutable candidate closure when
the publisher committed the plan. This is a preparation/control-plane timing
failure; it is excluded from M01 statistics.

## Bounded correction

`Experiments/spec175_repo_bootstrap.py` now exposes an explicit
`--ack-timeout-ms` for repository control-plane collaborations, defaulting to
5000 ms. The MiniNDN runner passes that value explicitly for publication and
Provider fetch. The registered streamed request/ACK/event deadlines are
unchanged. This gives the first SVS publication enough time to reach
`ACK_CLOSED` without changing streamed invocation semantics.

The helper regression and Spec175 contract/evidence/provenance tests pass after
the change. The temporary `SPEC175_FETCH` diagnostics used during localization
were removed before the current framework rebuild.

## M01 observations

| Run | Result | Interpretation |
|---|---|---|
| `focused-m01-eventloop-20260824a` | PASS | historical current-source diagnostic |
| `focused-m01-eventloop-20260824b` | PASS | historical current-source diagnostic |
| `focused-m01-eventloop-20260824c` | FAIL at cursor 1 | stream fetch race; no Interest/IMS receipt trace |
| `t022a-repro-20260824d` | setup-only | Provider artifact fetch stopped before stream |
| `t022a-repro-20260824e` | FAIL at cursor 1 | stream fetch race; no Interest/IMS receipt trace |
| `t022a-repro-20260824f` | setup-only | repository ACK closure had zero candidates |
| `t022a-repro-20260824g` | PASS | repository control-plane correction; M01 completed |
| `t022a-repro-20260824h` | PASS | M01 completed; late duplicate ACKs were ignored after completion |
| `t022a-repro-20260824i` | PASS | M01 completed |

The three `g/h/i` passes are diagnostic evidence, but they do not close T022-B:
they were collected while temporary fetch tracing was enabled and before the
post-diagnostic clean three-process repetition. The two cursor-1 failures
remain valid negative evidence and must not be pooled away.

## Clean M01 closure attempt

After the temporary diagnostics were removed and the current framework was
rebuilt, three independent clean processes were run with the explicit
repository ACK timeout and `NDNSF_SELECTION_TARGETED_PREFETCH=0`:

| Run | Result | Observed evidence |
|---|---|---|
| `t022b-clean-20260824j` | PASS | four ACKs, four selections, eight events/tokens, one final Response |
| `t022b-clean-20260824k` | PASS | four ACKs, four selections, eight events/tokens, one final Response |
| `t022b-clean-20260824l` | PASS | four ACKs, four selections, eight events/tokens, one final Response |

Each result is a separate process with `spec175-case-result.json` status
`PASS`; the user log records the exact token sequence `4,5,6,7,8,9,10,2`,
`events=8`, `duplicates=0`, and a validated final Response. These runs close
the T022-B M01 repetition packet, but not T022 or G3: the earlier c/e cursor-1
failures remain retained negative evidence, and M02--M10 still require the
registered three-process fault matrix.

Retain Provider Interest/IMS/pending/`face.put` instrumentation only if a new
cursor-1 failure reappears. Do not alter streamed retry/lifetime/deadline
values.
