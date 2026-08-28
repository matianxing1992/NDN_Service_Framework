# Research: NDNSF External Bug Report Corrections

## Decision 1: Treat Email Defects 1 And 2 As One Root-Cause Workstream

**Decision**: Reproduce and repair the current ndn-svs signed-inner/signed-outer
segmentation path, final packet sizing, commit ordering, and asynchronous error
containment.

**Rationale**: Current source still uses an 8000-B raw content threshold while
ndn-cxx limits the final encoded packet to 8800 B. Inner names, final-block
metadata, signatures, outer names, outer signatures, and TLV length fields make
raw content size an invalid safety proof. The two email symptoms share this path.

**Alternatives considered**:

- Lower the constant: rejected because name and signature overhead varies.
- Catch only the final exception: rejected because sequence state may already be
  advertised and Provider degradation would remain possible.
- Rely on automatic large-response externalization: rejected as a reproduction
  method because it bypasses the reported path.

## Decision 2: Freeze Existing Large-Response Reference Behavior

**Decision**: Do not redesign, harden, benchmark, or accept the existing
exact-name/reference mechanism in Spec 112. Use the existing
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1` flag only inside the isolated
diagnostic roles.

**Rationale**: The email reports SVS segmentation. Automatic externalization in
the current code can hide that failure, but the reference mechanism is not itself
one of the five reported defects.

**Alternatives considered**:

- Define production/reference/direct-object profiles: rejected as scope growth.
- Remove externalization: rejected because it changes unrelated production
  behavior.

## Decision 3: Preserve ndn-svs Public Publication Signatures

**Decision**: Repair internal preparation, storage, advertisement, and error
containment without adding `publishChecked`, changing return types, or introducing
a new public outcome state machine.

**Rationale**: The email asks for correct delivery and Provider survival, not a
new API. Existing callers may depend on current signatures, and internal ordering
can close the reported defects more narrowly.

**Alternatives considered**:

- Add checked sync/async APIs: rejected because no email requirement consumes
  them and they create new migration/callback semantics.
- Change `publish` return types: rejected as an unnecessary source break.

## Decision 4: Verify Current Python Targeted Behavior Before Editing

**Decision**: Test the real current binding with tokens enabled and one
`NormalAndTargeted` handler. Modify binding code only if the test fails.

**Rationale**: Current source already calls `setUseTokens(true)` for Provider and
User and registers `NormalAndTargeted`. Blindly applying the reporter's old
tokens-off-era patch would weaken current security.

**Alternatives considered**:

- Cherry-pick the reporter's fork: rejected because it targets older code.
- Expose `set_use_tokens(false)`: rejected as a security regression.

## Decision 5: Start Targeted Timeout At API Acceptance

**Decision**: Store and schedule one absolute deadline immediately after the
request is accepted and pending state exists. Response and timeout compete for
one terminal callback.

**Rationale**: Current source publishes before scheduling the timer, so degraded
publication/admission can escape `timeout_ms`.

**Alternatives considered**:

- Add a Python watchdog: rejected because C++ remains broken and callbacks race.
- Add new explicit-failure outcomes/protocols: rejected because the email only
  requires the existing timeout to fire unconditionally.

## Decision 6: Prove OpenABE Lifetime Before Changing It

**Decision**: Exercise 100 initialized subprocess lifecycles. Retain the current
process-wide executor/empty destructor if it passes; apply the smallest
same-thread, static-destruction-safe correction only if a crash reproduces.

**Rationale**: Source inspection cannot prove absence of SIGSEGV, but a speculative
shutdown rewrite can reintroduce the RELIC teardown ordering defect.

**Alternatives considered**:

- Always call guarded `ShutdownOpenABE`: rejected because a Boolean guard does
  not solve thread-local/static destruction order.
- Assume the current empty destructor is sufficient: rejected without executed
  lifecycle evidence.

## Decision 7: Restore Boost 1.71 As A Test Prerequisite

**Decision**: Make the local ndn-svs test configure/build accept the declared
Boost 1.71 baseline; keep any upstream documentation-only 1.74 rule separate.

**Rationale**: A stale test binary cannot validate source fixes, and the project
has explicitly retained 1.71 locally.

**Alternatives considered**:

- Upgrade the machine for this bug fix: rejected because it changes the candidate
  and contradicts the local baseline.

## Decision 8: Use A Single 0% MiniNDN Evidence Class

**Decision**: Run only the forced inline-SVS response path at 0% configured loss,
with Normal/Targeted × synchronous/asynchronous SVS publication. Bind every run
to one immutable candidate and preserve failures.

**Rationale**: This directly tests the email path with deterministic correctness
conditions. Wi-Fi, 5% loss, production-reference, and direct-object studies are
different experiments.

**Alternatives considered**:

- Add 5% loss observations: rejected because they confound correctness and were
  not requested.
- Re-run failed cells: rejected because negative results must remain intact; a
  fix creates a new candidate.
