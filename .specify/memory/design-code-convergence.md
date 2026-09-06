# NDNSF Design-Code Convergence Gate

This is the canonical pre-qualification checklist for durable NDNSF changes.
It applies after implementation work and before a test run is accepted as
formal validation or experiment evidence.

## Trigger

Run this gate for every feature, protocol/API change, architecture change,
distributed runtime change, security change, evaluation subject, or repair that
changes behavior-affecting source, dependencies, effective configuration,
harness logic, or evidence contracts.

## Inputs

- active feature pointer and exact `spec.md`, `plan.md`, `tasks.md`;
- versioned contracts, invariants, accepted design slides/docs, and prior audit;
- current source tree, production entry points, runtime launchers, and effective
  configuration;
- the actual compiler/linker and installed dependency headers/ABIs selected by
  the build (for example the resolved NDN-SVS header and library, not a newer
  checkout assumed from source history);
- focused development tests and available historical evidence;
- working-tree status and exact subject identity.

## Required Procedure

### Ordering invariant

The order is mandatory and may not be reversed:

```text
freeze design/configuration
  -> inspect production code and effective dependencies
  -> record and repair design/code gaps
  -> run focused repair regressions
  -> re-audit the repaired subject
  -> fresh PASS
  -> complete tests, qualification, benchmarks, and experiments
```

Starting a broad test run first and using its output to discover basic
design-to-code mismatches is not a valid shortcut. A broad run performed while
the verdict is `BLOCK` may be retained only as diagnostic history; it cannot
qualify the subject or authorize a later gate.

This review is also a prerequisite when planning a test: do not schedule a
complete suite, qualification run, benchmark, or experiment until the intended
production path has been traced and its known gaps have been repaired or
recorded as blockers. The only pre-`PASS` exception is a bounded focused test
whose purpose is to close a named repair finding; it must not be used to
discover the design/code distance or to justify starting a broader run.

### Required sign-off checklist

Before accepting any complete test, qualification run, benchmark, or
experiment, the operator MUST be able to check every item below. An unchecked
item is `BLOCK`, regardless of whether a previous run happened to pass.

- [ ] The active design and effective configuration are frozen and identified.
- [ ] CodeGraph and exact source inspection trace the real entry points,
      callers, wiring, security, dependencies, cleanup, logging, and evidence
      paths.
- [ ] Every design-to-code discrepancy has a severity, owner, correction, and
      closing focused regression; unresolved controlling gaps are recorded as
      blockers.
- [ ] The repaired production path has been re-inspected and the audit reports
      a fresh `PASS` for this exact source/build/configuration subject.
- [ ] The test uses the same executable, libraries, interpreter, artifacts,
      working directory, arguments, environment, and topology that the audit
      checked.

Focused red/green tests and the smallest bounded reproducer may run before
this sign-off only when they are closing a named finding. Their results are
development evidence, not formal validation.

1. **Freeze design authority.** Record the exact documents and decisions that
   control the implementation. Resolve contradictory written requirements
   before judging code.
2. **Inspect production reality.** Use CodeGraph first, then exact source
   verification, to trace public entry points through real callers, runtime
   wiring, security checks, dependency/configuration resolution, terminal
   ownership, cleanup, logging, and evidence emission.
3. **Verify the effective toolchain boundary.** Resolve the compiler, linker,
   include/library search paths, package versions, exported symbols, and ABI
   before a build or test is counted. Compile a smallest target against those
   exact inputs and remove or gate every source call to an API that the linked
   dependency does not expose. Do not treat a newer local checkout, stale
   object, or historical successful build as evidence for the current ABI.
4. **Build bidirectional traceability.** For every controlling requirement and
   success criterion, record its implementation owner, production caller,
   focused regression, formal validation case, and evidence field. Also flag
   code mechanisms with no requirement or user value.
5. **Separate evidence levels.** Mark each claim as specified, implemented,
   wired, executed, measured, or performance-qualified. A helper, checked task,
   isolated unit test, historical result, or successful component smoke does
   not prove production wiring.
6. **Record findings.** Each discrepancy needs severity, exact source evidence,
   affected requirement, failure consequence, correction, owning task, and
   closing regression.
7. **Repair before qualification.** Correct the specification/tasks first when
   design is incomplete or wrong; otherwise repair code to match the accepted
   design. Use focused failing tests, compile checks, and the smallest process
   reproducer during repair.
8. **Re-audit.** Inspect the repaired production path and focused evidence
   again. Formal validation is unlocked only by a fresh `PASS` verdict.

### Process and build-identity check

Every focused process reproducer must exercise the same production binary,
headers, shared libraries, interpreter, and runtime files that the repaired
path will use.  Before accepting its result, verify the executable hash,
`ldd`/RPATH closure, the effective working directory, and every subprocess
argument and environment variable.  A helper that imports an unrelated host
extension, resolves a relative artifact from an unverified directory, or loads
an undeclared package graph is a build/deployment discrepancy, not a test
failure to be worked around.  Repair that boundary, add a focused regression,
and repeat the code-aware audit before any broader run.  A clean component
smoke or a previously built object does not establish that the process uses
the current source identity.

## Verdict

- `BLOCK`: any unresolved semantic, architecture, security, production-wiring,
  cleanup, observability, or evidence-validity discrepancy that can invalidate
  the subject or its claims.
- `CONDITIONAL PASS`: only bounded non-controlling documentation or maintenance
  items remain, with explicit owners and no effect on validation meaning.
- `PASS`: design, production code, effective configuration, focused tests, and
  planned evidence agree for every controlling path.

## Testing Boundary

Allowed before `PASS`:

- test-first focused unit/contract cases for one repaired behavior;
- focused compile/link/import checks;
- the smallest bounded process reproducer needed to confirm a finding;
- deterministic mutation tests for the audit or closure gate.

Blocked before `PASS`:

- a complete unit or integration suite used as acceptance evidence;
- MiniNDN or other system/network qualification;
- SIF/container promotion or replay;
- TigerCluster/Slurm/GPU qualification;
- benchmark, long campaign, or paper experiment.

## Required Artifacts

- `evidence/design-code-audit-YYYYMMDD.md` or the feature's canonical
  code-aware `audit.md`;
- corrected requirements/tasks and dependency order;
- focused repair-regression evidence;
- final `PASS` report naming the exact source/design/configuration subject;
- invalidation/restart decision for any later change.

## Re-Audit Triggers

Re-audit after any behavior-affecting source, specification, contract,
dependency, model/artifact, effective-configuration, launcher, topology,
harness, security-policy, or evidence-schema change. Earlier formal evidence
then becomes historical for its old subject and cannot authorize the new one.
