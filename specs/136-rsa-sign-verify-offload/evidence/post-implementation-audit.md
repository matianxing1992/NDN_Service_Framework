# Spec 136 Post-Implementation Audit

## Verdict

`CONDITIONAL PASS` for freezing the once-only experiment and its
`TRADE_OFF` conclusion. There is no CRITICAL or HIGH issue that invalidates
the five SC-005 contrasts or the SC-006 verdict. Two MEDIUM evidence gaps
prohibit a complete FR-015 observability claim and production-safety
generalization; the frozen matrix must not be rerun to repair them.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A136-01 | MEDIUM | Validation/evidence | `spec.md:179-183`; `svs-rsa-single-worker.cpp:1030-1033,1148`; `analyze_svs_rsa_single_worker.py:241-244,383` | FR-015 requires full latency distributions, process CPU, Face commit residence, and failure reasons. The frozen subject records p99-only latency and maximum RSS; it does not retain the complete metric set. This does not change SC-005/SC-006, which use p99, delivery, security, and accounting. | Freeze the gap explicitly. Do not call FR-015 complete and do not rerun Spec 136. If these metrics remain necessary, instrument them before a separately numbered campaign. |
| A136-02 | MEDIUM | Security/distributed correctness | `spec.md:217-219`; `ndn-svs/tests/unit-tests/svspubsub.t.cpp:38-60,483-558,910-940,1038-1058,1138-1154` | The 27-case suite verifies default-off/single-only configuration, off-Face preparation with ordered Face commit, a signing failure on the inline path, and shutdown rollback. It does not directly inject publication-worker queue saturation or a signing failure while worker mode is enabled, so SC-003's worker failure boundary is only code-reviewed, not fully executed. | Retain the limitation and forbid production-safety claims. A later Spec may add focused worker failure injection; the formal performance cells remain immutable. |

## Traceability Gaps

| Source/Requirement/Task | Missing link | Impact |
|---|---|---|
| FR-015 -> T004/T006 | No retained p50/p95/max distributions, process CPU, or Face commit residence | Complete observability claim is unsupported; SC-005 p99 verdict remains computable. |
| SC-003 -> T003/T004 | No direct worker-mode queue-full or worker-signing-failure execution | Code path is implemented and bounded, but failure safety is not promoted from code-reviewed to executed. |

No task or implementation mechanism lacks user value: the one-worker option,
independent pacer, RSA validation, Fetch-window control, Mapping de-duplication,
runner, and analyzer all map to the registered experiment or a discovered
harness-correctness fault.

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Same binary, inline versus exactly one worker, two-node bidirectional RSA PubSub. |
| Architecture and ownership | Yes | Preparation moves off Face; commit/state/I/O remain Face-owned. |
| Security/correctness | Conditional | RSA tamper probes and network validation pass; A136-02 limits failure-boundary claims. |
| Task executability | Yes | All six tasks have executed artifacts and dependency order. |
| Task cohesion/granularity | Yes | One implementation task, one admission task, one formal task, one closure task. |
| Validation/evidence | Conditional | Ten formal receipts and five contrasts are valid; A136-01/A136-02 remain. |
| Migration/rollback | Yes | Default worker count is zero; rollback is disabling the opt-in worker. |
| Code reality | Yes | CodeGraph and the 27-case `TestSVSPubSub` run confirm the actual worker/Face path. |

## Metrics

- User stories: 4
- Functional requirements: 21
- Success criteria: 8
- Tasks: 6
- Mechanically fragmented task groups: 0
- Coalescing opportunities: 0
- Requirement coverage: 21/21 have task/evidence paths; 2 are partial
- Unmapped tasks: 0
- Placeholders: 0
- Critical / High / Medium / Low findings: 0 / 0 / 2 / 0

## Code And Test Reality

- `SVSPubSub::publishAsync(bytes)` copies its input and selects the preparation
  worker only when configured.
- `enqueuePublicationPreparation()` checks the bounded slot before reserving a
  sequence, records accepted/outstanding state, and never falls back inline.
- One worker performs inner/outer construction, encoding, and Data RSA signing;
  the prepared result returns to Face for ordered commit.
- Sync Interest signing and receive-side validation remain on the serial Face
  path in the registered benchmark.
- `LD_LIBRARY_PATH=/home/tianxing/NDN/ndn-svs/build ./build/unit-tests -t
  TestSVSPubSub --log_level=test_suite` passed 27/27. Running without that
  library path resolved the older installed `/usr/local/lib/libndn-svs.so` and
  was rejected as a stale-linkage invocation, not counted as a test result.
- R6 preflight passes RSA valid/tampered Data and Interest checks, four
  60-second 1000-pps no-op peer checks, and both fresh 200-pps two-node smokes.
- The formal R6 campaign has 10/10 unique `COMPLETE` receipts and zero analyzer
  validation errors.

## Assumptions And Evidence Limits

- One cell per condition supports bounded descriptive contrasts, not p-values
  or population inference.
- Results apply to this four-core host, two-node topology, RSA implementation,
  10/60/10 window, zero configured loss, and 200–400 pps/peer range.
- The 400-pps delivery-p99 gain coexists with a heartbeat-p99 regression.
- Missing FR-015 metrics cannot be reconstructed from the retained summaries.
- The R3 400-pps descriptive confirmation and all Spec 135 evidence remain
  historical; neither is substituted into the formal matrix.

## Next Actions

1. Freeze Spec 136 and its R6 campaign with the `TRADE_OFF` verdict.
2. Do not rerun, replace, tune, or selectively supplement any of its ten cells.
3. If the research question continues, define a new Spec before adding full
   CPU/latency distributions or worker-specific fault injection.
