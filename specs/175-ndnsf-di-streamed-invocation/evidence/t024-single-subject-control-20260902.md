# T024 single-subject Tiger control correction — 2026-09-02

## Finding

Repeated Tiger failures were not caused by one stable experiment failing in
different ways. Earlier launches changed the effective subject between runs:
ambient `--export=ALL` values were not always consumed, relative artifacts were
resolved from an unverified working directory, host-built extensions crossed
the SIF ABI boundary, helper/configuration/SIF repairs were mixed with old
evidence, and the external campaign watchdog could stop a case before a
terminal manifest existed. A new job number therefore hid configuration drift
instead of reproducing the earlier successful subject.

## Contract correction

Spec175 now defines one checked-in launcher and profile as the source of truth.
The profile owns the command, environment, working directory, mounts, routes,
identities, resources, readiness order, timeouts, workload, and cleanup. A
matrix is generated as data from declared axes. Run-level rows may reuse an
unchanged sealed candidate; subject-level changes (for example coverage,
speed, timeout, topology, resource, Provider/GPU count, workload, or
model/runtime) require a new profile digest and the owning local gates. An
undeclared or unconsumed value fails before any external side effect.

The replay driver owns a per-case and campaign watchdog, launches each case in
a fresh process group/output directory, requires all four Provider readiness
records, reaps descendants on timeout, and emits `firstIncomplete` plus
`NOT_RUN` rows on interruption. Partial counts are diagnostic only. Any
behavior-bearing driver/profile/dependency change invalidates the source/SIF
chain and restarts at the earliest gate.

## Verification

- `tests/python/test_spec175_replay_driver.py`
- `tests/python/test_spec175_tiger_profile.py`
- `tests/python/test_spec175_tiger_checklist.py`
- `tests/python/test_spec175_sif_preflight.py`
- Result: **46 passed**.
- `quick_validate.py /home/tianxing/.codex/skills/itiger-ndnsf-ops`: **valid**.
- Spec175 strict structure audit: **PASS** (82 requirements, 21 criteria,
  42 tasks; 36 currently closed).

This correction does not qualify the old SIF or the earlier 29/42 replay. A
fresh source seal, G0–G3 chain, candidate SIF, and complete G4 replay remain
required. The current fresh chain is independently blocked at G1 by the
NDN-SVS header/shared-library feature mismatch recorded in
`t020-g1-svs-api-parity-20260902.md`.
