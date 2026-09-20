# Spec189 r141 initial producer-readiness boundary — 2026-09-20

## Changed gate

r140 showed Provider-1 entering the production dependency fetch for Provider-0's
exact tensor manifest while Provider-0 was still materializing selected model
materials. The consumer's first manifest fetch was bounded by the same
`noProgressDeadlineMs` intended for progress after publication, so the run
exhausted exact signed-data attempts before `EXACT_DATA_PUBLISHED`.

The changed gate is the C++ `NDNSF_DATA_V1` V3 dependency deadline contract in
`NdnsfCollaborationDependencyIo::prefetchInput`: the first manifest fetch is
bounded by the remaining request hard deadline and dependency fetch budget;
after the manifest is received, each segment fetch remains bounded by the
ordinary no-progress window and dependency fetch budget. Hard deadline,
terminal/cancel checks, producer identity and manifest validation are unchanged.

## Red/green evidence

- First test-only build attempt: failed before linking because the new test used
  unqualified `BootstrapProfile`, `NdnsfIntegrationEnvironment` and
  `EnvironmentStatus` names. Raw output:
  `.codex-tmp/spec189-r141-initial-readiness-regression/build-test-only.log`.
- Repaired build: Waf `integration-tests`, `127/127`, raw output
  `.codex-tmp/spec189-r141-initial-readiness-regression/build-test-only-r2.log`.
- Red test against the old implementation: delayed producer publication was
  not reached before the 100 ms no-progress budget; the new C++ selector failed
  at `critical check published has failed`, rc `201`. Raw output:
  `.codex-tmp/spec189-r141-initial-readiness-regression/red-test-before-fix.log`.
- Repaired build: Waf `integration-tests`, `127/127`, raw output
  `.codex-tmp/spec189-r141-initial-readiness-regression/build-after-fix.log`.
- Green focused test: the new readiness case passed, rc `0`. Raw output:
  `.codex-tmp/spec189-r141-initial-readiness-regression/green-test-after-fix.log`.
- V3 regression selector: the existing manifest/segment case and the new
  readiness case both passed; the log reports `Running 2 test cases` and
  `*** No errors detected`. Raw output:
  `.codex-tmp/spec189-r141-initial-readiness-regression/v3-regression-suite.log`.

## Review and limits

An immutable source snapshot was frozen before the read-only review under
`.codex-tmp/spec189-r141-initial-readiness-regression/static-review-20260920/`;
it records base commit `9726c7f78e0063f036057d04e6cb4360eb7da0a5` and SHA-256
digests in `SHA256SUMS`. The read-only review returned `STATIC_PASS` with no
blocker and no file changes. It recorded three P2 follow-ups for later negative
coverage: an unpublished manifest must stop at hard deadline/fetch budget, a
fetch budget smaller than the hard deadline must cap the initial wait, and a
post-manifest segment stall must still stop at the no-progress bound. These are
not runtime qualifications and do not change the focused checkpoint verdict.

This evidence records a focused C++ repair, not a MiniNDN or model
qualification result. The next gate is affected target installation and
identity verification, followed by a new real run ID. T003, T005, T006, T007
and T009 remain `PARTIAL`.
