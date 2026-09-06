# Spec180 Design/Code Conformance Audit — Revision 123 (2026-09-04)

Scope: the current working tree against the revision-123 documents
(`spec.md`, `plan.md`, `traceability.md`, contracts, `data-model.md`).
Report-only audit per the 12 speckit-audit principles; no fixes applied.
Four layers are reported separately. Code evidence verified with CodeGraph
(verbatim current on-disk source) and direct reads; citations are file:line.

## Verdict

**CONDITIONAL PASS (implementation may continue; the fixes below MUST
complete before a fresh T014 convergence claim, and H1 before any new
candidate seal).**

The revision-112/123 design is bounded, internally coherent, and matches
intent: one immutable YOLO vertical slice, ACK-driven placement, no
performance claims, Qwen/cross-model deferred. The revision-123 readiness
repair is correctly implemented and its ordering guarantees actually hold on
the production runner path. No CRITICAL or design-overturning issue was
found. The open issues are evidence-integrity, audit-pointer, and two
documentation-consistency items, plus one marker-ambiguity robustness nit.

## Layer 1 — 文档声称 (proposed)

Consistent and credible: `spec.md:16-39` (rev 123), `spec.md:41-110`
(rev 112 authoritative), `plan.md:5-49`, contracts updated to the deferred
cross-model boundary (`contracts/cross-model-qualification-v1.md:4-5`),
`data-model.md:67-95` matches FR-022 (`ciphertextDigest` aliasing corrected).
Gate order S0–S5 is explicit and only the first open gate is active
(`spec.md:66-80`). Claims found inconsistent:

- **M1 (MEDIUM) — audit pointer disagrees across three documents.**
  `spec.md:9` says "audit iteration 123"; `traceability.md:31-32` still says
  "audit iteration 105, documentation revision 112, 2026-09-03"; `audit.md`
  has no iteration-123 section at all (newest section is "Revision 112 audit
  decision" at `audit.md:14`). The A180-123 HIGH findings in `audit.md:582`,
  `:603`, `:628` describe `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`, which no
  longer exists in the runner (see Layer 2) — the closure of that finding is
  not recorded in `audit.md`.
- **M3 (MEDIUM) — `traceability.md:115` says "fixed Tiger profiles, two
  jobs"** while FR-017 (`spec.md:1463-1466`) defines exactly one cold Y-B
  request and FR-020 (`spec.md:1483-1487`) allows only one byte-identical
  resubmission for a recorded Slurm/host failure. Pre-revision-112 residue.
- **L3 (LOW) — `audit.md` tail says "T014--T020 are not started"** while
  `tasks.md:1020` shows T019 (scope-transfer) checked complete.

## Layer 2 — 代码实现 (implemented)

Verified conforming:

- **Revision-123 repair chain is complete and wired.**
  Core probe: `ndn-service-framework/ServiceController.cpp:226-275`
  (heap-owned `shared_ptr` state, 10 s deadline, throw on timeout).
  Native propagation: `pythonWrapper/src/ndnsf/_ndnsf.cpp:3926-3943`
  (`start()`), `:4006-4031` (`runControllerLoop` sets `m_started`/`m_error`,
  notifies, rethrows when `propagateError`), `:3945-3964`
  (`waitUntilReady`). Python fail-closed wrappers:
  `pythonWrapper/ndnsf/service.py:2303-2334` (both `start()` and
  `start_background()` wait 10000 ms, stop and raise on timeout/error).
  Handler registration is idempotent via `m_isRegistered`
  (`ndn-service-framework/ServiceController.cpp:421-424`).
- **Probe is servable by the real AA endpoint.** NAC-ABE names it
  `/AA-prefix/PUBPARAMS` (`/home/tianxing/NDN/NAC-ABE/src/attribute-authority.cpp:144`)
  and sets `FreshnessPeriod(5_s)` (`:151`), satisfying the probe's
  `MustBeFresh` Interest.
- **The readiness ordering holds on the production runner path.**
  `examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py:137`
  calls `start_background()`, which blocks until the probe completes
  (Layer-2 repair above); only then prints the marker at `:145` and, after
  signing and readback, `SPEC180_RUNTIME_CATALOGUE_PUBLISHED` at `:155`.
  The runner uses that post-publication marker as the controller readiness
  signal (`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:740-742`) and
  releases the repository launch only after it (`:1066-1068`). The
  rev-122 race is therefore closed on the real path.
- **FR-003/FR-004.** V3 configuration boundary rejects caller ACK coverage:
  `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:628-635`.
  Candidate priority/digest tie-break:
  `.../app_sdk/placement.py:1787-1798` (`(-priority, candidate.candidate_digest)`).
- **FR-025 runner contract.** `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:984-1075`
  rejects unknown/duplicate phases, missing started AND ready predecessors,
  requires catalogue publication before `user`, resolves all nodes before the
  first child, and is phase-atomic on failure. No call to the legacy
  `NDNSF_DI_Yolo2x2_Minindn.py` `main()` exists; only the maintained
  MiniNDN helpers are reused via `legacy.start(...)`. Pre-network input
  failures emit `WAITING_EXTERNAL_INPUT` (`:2474`, `:2483`).
- **FR-022 codes** (`DI_INPUT_FETCH_ROLE_MISMATCH`,
  `DI_TERMINAL_RESPONSE_ROLE_MISMATCH`, `DI_INPUT_REPO_DIGEST_UNVERIFIED`)
  exist in `ndnsf_distributed_inference/provider.py`.

Findings:

- **M4 (LOW-MEDIUM) — fallback readiness marker is ambiguous.** When no
  publication file is configured, the runner's controller marker is the
  string "ServiceController listening" (`runner:742`). That substring also
  matches the C++ log emitted at the END of `registerInterestHandlers`
  (`ndn-service-framework/ServiceController.cpp:509-515`), which runs
  BEFORE the PUBPARAMS probe (`:228`). In that fallback path the barrier
  can release before the probe completes. The real driver always builds a
  publication file (`runner:2389-2390`) and uses the unambiguous
  `SPEC180_RUNTIME_CATALOGUE_PUBLISHED` marker, so this is not hit today;
  still, the fallback marker should be a distinct string printed only after
  `start_background()` returns (post-probe).
- **L1 (LOW) — equal deadlines race.** Python waits 10000 ms
  (`service.py:2306, 2327`) against the C++ 10 s probe deadline
  (`ServiceController.cpp:243-244`); a probe completing exactly at the
  boundary can be reported as a wrapper timeout. Give the wrapper a margin
  (e.g. 15000 ms).
- **L2 (LOW) — `stop()` during an in-flight probe hot-spins.**
  `stop()` (`_ndnsf.cpp:3985-3995`) stops the io context and joins; the
  probe loop (`ServiceController.cpp:245-269`) then calls
  `processEvents(50 ms)` on a stopped context (immediate return) until the
  10 s deadline. Functionally correct (fail-closed), but burns CPU for up
  to 10 s during teardown.

## Layer 3 — 测试执行 (executed)

- `evidence/t013-controller-pubparams-readiness-current-20260904.md`
  honestly records: 274/274 `-j2` build PASS, 22 focused tests PASS,
  extension import PASS, and the rev-122 SIF failure (`exitCode=7`) as
  negative evidence. It explicitly declares itself
  implementation/wiring evidence, not qualification — correct layer
  discipline.
- No live MiniNDN case has run since the repair (S2 unexecuted). The
  probe's live behavior (real NFD loopback, freshness satisfaction) is
  verified only by static analysis above; T015 must exercise it.

## Layer 4 — 实验测量 (measured)

- S0 native closure: recorded (`evidence/s0-native-closure-current-20260903.md`,
  `tasks.md:253`).
- S1 seal (`evidence/s1-candidate-seal-current-20260904.md`) and T016
  exact-SIF Y-B replay (`evidence/t016-exact-sif-yb-replay-current-20260904.md`,
  SIF `c7c84006…`) are rev-121/122 artifacts invalidated by the rev-123
  runtime-plane change. The invalidation is recorded in t013 and the spec
  header but **not inside those two files themselves** —
  **M2 (MEDIUM)**: a reader of t016 alone sees `PASS`; add an
  invalidation banner to every superseded evidence file (frozen-evidence
  hygiene, principle 10).
- **H1 (HIGH) — the entire feature state is outside git.** The whole
  `specs/180-ack-driven-cross-model-qualification/` tree, the runner
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`, and the revision-123
  repair files (`ndn-service-framework/ServiceController.cpp`,
  `pythonWrapper/ndnsf/service.py`, `pythonWrapper/src/ndnsf/_ndnsf.cpp`)
  are untracked/uncommitted (`git status`), while sibling feature trees
  (e.g. specs/175) are tracked. There is no git-immutable record of any
  Spec180 contract, audit iteration, or evidence; FR-015's source binding
  and the frozen-evidence rules (principles 9/10) are unenforceable in this
  state. Commit the documents and the repair before any new candidate seal
  or T014 claim.

## Required before the next T014 convergence claim

1. H1: commit specs/180 + runner + repair code; record the commit hash.
2. M1: unify the current-audit pointer (spec header / traceability /
   audit.md) and record the closure of the A180-123 NOT_WIRED finding.
3. M2: add invalidation banners to t016 and s1 evidence files.
4. M3: correct `traceability.md:115` to one Tiger job (plus the single
   permitted byte-identical resubmission).
5. M4/L1/L2 recommended before T015 (fallback marker disambiguation,
   wrapper deadline margin, probe teardown spin).

## Appendix — Historical SIF+Tiger success and reproducibility audit

Question audited: the project once had SIF+TigerCluster successes, later
changes became hard to reproduce; was the SIF+scripts pinned, and will the
same configuration with parameter-only script changes reproduce?

### What the historical success actually was (all Spec175)

| Job | Date | Candidate (SIF SHA-256) | Gate | Result |
|---|---|---|---|---|
| 206901 | 2026-08-29 | `spec175-runtime-fe285147` (`6d905dbb…`) | control (4-provider HELLO) | **PASS** (`t025-tiger-control-206901.md`) |
| 206907 | 2026-08-29 | `spec175-runtime-fe285147` (same SIF) | G5 stage readiness (Qwen3.6-27B, 3×RTX6000, CUDA, 8 tokens) | **PASS** (`tiger-stage-readiness.md`) |

Both successes are on ONE candidate (`fe285147`, source `15f823a4`), via the
pre-hardening submit path. The functional `multi-provider` gate NEVER
succeeded on Tiger; three attempts failed with three different root causes:
`206795` (registration ordering: Controller printed READY before
`APPController.start()`), `206910/206915` (Controller face closed → Provider
certificate bootstrap Nack 150, same fe285147 SIF), `208200` (candidate r5:
"automatic planning candidate digest mismatch").

### Audit findings

- **R1 (CONFIRMED, then fixed by the project itself).** The successes were
  submitted through the old ambient-env path (`sbatch --export=ALL`); no
  byte-pinned submit closure existed at the time. `t024-proven-tiger-profile-20260901.md`
  explicitly diagnoses this: retries "reproduced the same defect under new
  job IDs". The user's suspicion was correct, and Spec175 T024 retrofitted
  the fix: `contracts/tiger-experiment-profile-v1.md`,
  `proven-tiger-profile.json` (hashes every helper/wrapper/sbatch/validator),
  render-then-submit exact-delta, `--export=NONE`.
- **R2 (HIGH) — the only successful SIF has no local copy.** `fe285147`
  `runtime.sif` exists only at
  `/project/tma1/ndnsf-di/releases/spec175-runtime-fe285147/runtime.sif` on
  Tiger storage; `.codex-tmp/spec175-final-candidate-fe285147/` keeps the
  build record and sources but not the image. The successful submit closure
  was never archived as a proven baseline (the machinery came after). Local
  disks hold only r5/post-contract/r119/r24 SIFs. Local reproducibility of
  the historical success therefore depends on Tiger-side storage surviving.
- **R3 (design answer to the user's hypothesis).** "Same configuration plus
  parameter-only script changes reproduce the success" is DELIBERATELY NOT
  a supported expectation, and that is correct for this pipeline:
  - Spec180 `scripts/spec180_candidate.py:29-47` invalidation matrix: a
    `profiles` change invalidates from `REMOTE_READINESS`, a `workloads`
    change from `LOCAL_QUALIFICATION`, and every later plane.
  - FR-020 (`spec.md:1483-1487`): only one byte-identical resubmission is
    allowed, and only for a recorded Slurm/host failure before workload
    entry; any source/model/profile/launcher/SIF/workload/oracle change is a
    NEW candidate returning to its earliest invalidated gate.
  - Spec175 `t024` Restart boundary: "No historical SIF, job number, or
    changed-parameter retry can authorize Tiger."
  The correct operating model is: freeze everything at a commit → build →
  exact-SIF local replay → ONE gated submission; a parameter change does not
  inherit the old success, it restarts qualification. The machinery to
  enforce this exists in code (`spec180_release.py`: render is side-effect
  free, `profileSha256` binding, SIF/model/workload digest verification,
  `submit.sh` is the only sbatch path; `build-local-sif.sh` requires
  `--source-seal` + `--host-gate-manifest`), but it is only meaningful once
  the tree is committed (H1) — today "same configuration" is undefined
  because the whole feature state is uncommitted working-tree bytes.
- **R4 (LOW) — deferred QWEN-F gate still accepted by the boundary.**
  `spec180_release.py:28,50` and `submit.sh` accept `qwen-functional`, which
  Spec180 defers (FR-012). The QWEN-F path verifies a signed external
  manifest before scheduler mutation (`spec180_release.py:157`), so it is
  guarded, but the deferred gate should be explicitly marked deferred in
  `profile.json`/release output rather than look submittable.
- **R5 (note) — no precedent for the functional gate.** Both Tiger successes
  were control/stage gates; the spec180 `yolo-functional` gate is new and
  has never passed on Tiger. There is no historical basis for expecting it
  to succeed on a parameter tweak; the honest expectation is the rev-112
  route: S0→S5 with each gate's evidence freshly produced.

### Recommendations

1. Commit the whole tree (H1) — reproducibility cannot be claimed over
   uncommitted bytes; record the commit in the candidate seal.
2. Preserve the successful artifact closure: copy `fe285147` `runtime.sif`
   from Tiger storage to local archive, hash-verify it against
   `6d905dbb…`, and archive the submit closure (scripts, profile, env,
   run-record) that produced jobs 206901/206907 with hashes — as the proven
   baseline for future control/stage gates.
3. Optionally re-run the fe285147 control gate once as a reproducibility
   anchor through the current hardened `render/submit` path (one bounded
   allocation); this converts the historical PASS into a provenance-complete
   baseline. Do NOT attempt this with any parameter drift.
4. Keep the invalidation matrix as the only promotion path: parameter-only
   script changes must produce a new candidate and restart at the earliest
   invalidated gate — never inherit the old success.
5. Mark `qwen-functional` deferred in the profile/release boundary (R4).
