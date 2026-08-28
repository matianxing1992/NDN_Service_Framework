# Tasks: Single-Worker RSA Publication Offload

**Input**: R3 controlling documents in
`specs/136-rsa-sign-verify-offload/{spec.md,plan.md,contracts/,checklists/}`

## Phase 1: Revised Readiness Gate

- [x] T001 **SUPERSEDED BY T002** — R0 audit of the former two-binary,
  broad validation, and ordered multi-completion design. Retained only as
  history; it is not R1 implementation-readiness evidence.
- [x] T002 Audit the R1 documents against current NDN-SVS source, verify the
  one-binary/two-mode boundary, two-node bidirectional workload, real RSA
  requirements, one-worker ownership, exact ten-cell matrix, claim rules, and
  Spec 135 freeze. A HIGH/CRITICAL finding blocks T003.

## Phase 2: Implement The Minimal Same-Binary Intervention

- [x] T003 [US2] Implement a
  default-off `0|1` publication-preparation-worker option with a bounded FIFO,
  copy-owned jobs, one active RSA publication signer, Face-owned commit/state,
  explicit queue/failure/shutdown accounting, and unchanged default
  `publishAsync()` behavior. Configure persistent RSA-2048 Data and Sync
  Interest signing plus real peer validation in the benchmark; do not modify
  piggyback behavior or unrelated Sync worker paths. Per the 2026-07-23 scope
  decision, stop expanding the unit-test matrix; use the real two-node
  MiniNDN path for the remaining admission evidence.

## Phase 3: Build And Admit One Binary

- [x] T004 [US1] Build and freeze one binary; implement the two-node
  bidirectional MiniNDN runner and analyzer; prove both peers simultaneously
  publish and subscribe, runtime modes differ only by worker count 0 versus 1,
  each peer uses an independent APP pacer, control posts calls to Face,
  treatment directly calls byte `publishAsync()` from APP, rates are per peer,
  real RSA/tamper checks pass, accounting and routes pass, and the no-op 1000
  pps/peer pacer is within +/-2% separately on both peers. Seal the exact
  ten-cell manifest only after every preflight passes.
  - [x] T004a Implement the runner and analyzer and pass a fresh two-cell
    `face-inline-rsa`/`worker-rsa` MiniNDN smoke with real RSA, two-way
    publish/subscribe, zero validation errors, and correct `0|1` worker
    evidence.
  - [x] T004b Replace the invalid Face-timer harness with an independent APP
    pacer, enforce mode-specific caller-thread evidence and per-peer attempted
    admission, complete the remaining tamper and 1000 pps no-op pacer checks,
    then seal the formal manifest. The earlier asymmetric smoke remains
    `HARNESS_INVALID`; fresh short smoke is not formal performance evidence.
    - Retained R2 smoke `smoke-20260723T202803Z` passes attempted-rate and
      caller-thread admission for all 12 peer results at 400/800/1000 pps, but
      has zero measured delivery and 409-2760 outstanding publications per
      peer. T004b remains open; this smoke cannot seal or justify the formal
      matrix and MUST NOT be retried or relabeled.
    - Fetch diagnostics exposed that the harness subscribed to the shared
      parent namespace and therefore fetched its own publications before
      discarding them in the callback. This violated the existing A-subscribes-
      to-B/B-subscribes-to-A contract. The failed diagnostic
      `smoke-20260723T204126Z` is retained.
    - Corrected smoke `smoke-20260723T204437Z` uses exact remote-peer
      subscription prefixes and reports zero self deliveries. At 400 pps/peer,
      both modes satisfy attempted admission. Inline delivers zero measured
      publications with 483.638 ms heartbeat p99; the one-worker mode delivers
      738 measured publications (123 pps/peer, 30.78%) with 88.875 ms heartbeat
      p99. This is positive short-smoke evidence for Face relief, not formal
      performance evidence.
    - Corrected `smoke-20260723T204551Z` retains the 800/1000 pps boundary:
      attempted admission passes on every peer, but both modes deliver zero.
      These common-collapse cells cannot establish a worker regression or seal
      the formal campaign. Tamper/no-op admission and formal sealing remain
      open.
    - R3 audit correction: the 800/1000 cells are `OVERLOAD_INVALID`, not a
      measured Sync/Fetch ceiling. At 400 pps the measured two-Data plus
      one-Interest RSA demand implies an approximately 626.6 pps/peer serial
      signer ceiling; 800/1000 demand 127.7%/159.6% utilization before other
      Face work.
    - R3 implementation MUST enable identical 5 ms Sync batching, split signer
      mutex wait from crypto service, enforce <=90% signer-utilization and
      complete-drain admission, retain 1000 pps only as a no-op pacer check,
      and replace the unexecuted formal matrix with
      200/250/300/350/400 pps per peer.
    - R3 600-pps diagnostic found asymmetric Face starvation despite admissible
      signer demand. A targeted 16-commit-per-turn probe did not restore
      measured delivery and merely flipped the starved peer, so that Core
      change was removed. The retained probe is diagnostic evidence that the
      four-core process budget, not commit batching, controls this boundary.
    - R3 smoke `smoke-r3-sustainable-20260723T212102Z` is valid only at
      300 pps. Both 400-pps modes are `LOAD_UNSUSTAINED`; the earlier positive
      interpretation of 0 versus 79.5 pps/peer is withdrawn.
    - Diagnosis found repeated Mapping announcements registering the same
      pending subscription five times. The new regression fails `5 != 1`
      before the de-duplication fix and passes afterward. A separate 128-item
      worker test proves preparation off Face, ordered commit on Face, and zero
      outstanding work.
    - The common publication Fetch window is now 64 in both modes, avoiding the
      unrelated 10-slot receive bottleneck. Fresh 10/60/10 confirmation
      `confirmation-400-r3-fixed-20260723T220300Z` records exact 400 pps/peer
      attempted load in both modes. Inline sustains 231.44 pps/peer and is
      `LOAD_UNSUSTAINED`; worker sustains exactly 400 pps/peer with zero Fetch
      timeout and zero outstanding work. This is a focused 400-pps capacity
      confirmation, not the formal ten-cell matrix.
    - Post-implementation claim audit classifies that confirmation as
      `NON_FORMAL_DESCRIPTIVE`. The analyzer now preserves the recorded
      terminal beside its interpretation, reports duplicate callbacks and
      heartbeat skipped ticks, and marks delivery p99 `delivered-only` when
      paired delivery ratios differ by more than one percentage point.
    - R5 preflight
      `preflight-r5-20260723T223833Z` passes the independent tampered
      Data/Interest probes on both peers, all four 60-second two-peer no-op
      pacer checks at exactly 1000 attempted pps, and fresh inline/worker
      200-pps bidirectional MiniNDN smokes with 100% unique delivery, real RSA
      validation, and zero outstanding or abandoned work. The R5 build and
      exact ten-cell manifest are sealed. No formal cell was run or relabeled.
      R4 is retained as superseded because its immutable manifest incorrectly
      made future explicit T005 execution impossible.
    - R6 preflight
      `preflight-r6-20260723T224509Z` repeats the complete T004b gate after
      removing R5's formal-loop fail-fast. R6 preserves every failed receipt
      while continuing through the fixed matrix. Its security, two-peer no-op
      pacer, and fresh 200-pps MiniNDN checks all pass; the R6 build and exact
      ten-cell manifest are sealed. No R4/R5 formal cell was executed.

## Phase 4: Execute Once

- [x] T005 [US3] Run the sealed ten cells once in contract order using
  10/60/10 timing. Preserve every terminal receipt and do not retry, replace,
  tune, or selectively omit any formal cell.
  - R6 campaign `formal-r6-20260723T224836Z` contains exactly ten unique
    receipts in the sealed order. All ten are `COMPLETE`; no retry or
    replacement directory exists.

## Phase 5: Analyze And Freeze

- [x] T006 [US4] Produce five paired contrasts and apply SC-005/SC-006.
  Separate Face relief from RSA service demand, retain negative outcomes, run
  the post-implementation audit, and freeze Spec 136.
  - The formal analyzer reports `NOT_USEFUL_AT_RATE` at 200/250/300/350 and
    `USEFUL_AT_RATE` at 400. Because usefulness is not present at two adjacent
    rates and neither mode misses offered load, SC-006 yields the bounded
    overall `TRADE_OFF` verdict. The conditional post-implementation audit and
    freeze retain the incomplete-metric and failure-injection limitations.

## Dependencies

```text
T001 (superseded history)
T002 -> T003 -> T004 -> T005 -> T006
```

T003 is the only implementation task. T004 cannot seal the campaign until the
focused correctness/security/harness preflights pass. T005 is the only formal
execution task. All tasks are closed; later evidence gaps require a new Spec
and MUST NOT trigger a Spec 136 formal rerun.
