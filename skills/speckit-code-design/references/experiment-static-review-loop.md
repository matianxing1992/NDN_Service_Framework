# Experiment Static Re-review Loop

Use this loop for any experiment that exercises a real production request,
cross-process protocol, model execution, resource boundary or qualification
candidate. It is a repeated gate around the experiment lifecycle, not a new
administrative task for every retry.

## Loop

1. **Freeze before the run.** Record the source/build/ABI/model/profile,
   launcher and selector identities, output directory, resource floors and
   expected event sequence. The static gate reads the real launcher, generated
   configuration/policy, production callers, C++ selector/oracle, test
   registration and build/source closure. It also checks the negative and
   cleanup paths that the run can exercise.
2. **Run one bounded attempt.** Preserve raw stdout/stderr, child exit status,
   resource samples and the exact candidate/run tuple. A harness start,
   admission response, `--help`, or static selector result is not a product
   execution result.
3. **Classify the first boundary.** Name the last observed production marker
   and the first missing marker. Separate preflight, compile/link, runtime/test,
   resource and protocol boundaries. A generic timeout or stream gap is a
   transport symptom until the producer-side boundary is observed.
4. **Update durable state before retrying.** Add the raw record to the active
   Spec evidence and failure index, update `tasks.md` and keep incomplete work
   `PARTIAL`/`BLOCKED`/`UNQUALIFIED`. Do not overwrite the prior run or reuse
   its PASS status.
5. **Declare a Changed gate.** Identify a real change that covers the first
   boundary: production code, caller/default wiring, fixture/driver, generated
   policy or credentials, test/oracle registration, build/source closure,
   resource guard, or a deliberately changed counterfactual. Changing only a
   run id, timeout, log level or copied digest is never a Changed gate. A
   candidate-derived digest checker, manifest or preflight implementation is a
   gate only when the new check itself covers the first failure boundary.
6. **Freeze and re-review.** Create an immutable snapshot containing the new
   or modified files, complete diff and surrounding production code. The
   read-only review-agent rechecks the Changed gate, all affected callers,
   invariants, tests/harness/oracle, build registration and the negative path.
   Re-review any shared interface or ownership invariant that the repair could
   affect. Do not build or run while this gate is unresolved.
7. **Rebuild and rerun only the affected scope.** After static re-review passes,
   rebuild the changed target/source closure, run the named C++ selector and
   then the bounded experiment. Record compile/link and runtime misses
   separately from the previous attempt.
8. **Escalate repeated misses.** If the same class of miss recurs, revise the
   shared skill, template, checklist or experiment harness before another full
   retry, or record a concrete equivalent gate and keep the result open.

## Minimum result record

The active Spec keeps one result record for the loop with:

- `Attempt`: unique run/preflight id and raw evidence paths;
- `First boundary`: last observed marker, first missing marker and classification;
- `Changed gate`: exact changed files/symbols, reason and coverage lane;
- `Review trace`: review-agent path/SHA, immutable base, diff scope, queries,
  findings and re-review result;
- `Build/test`: target/source closure, toolchain, `-j`, elapsed, selector and
  exit code;
- `Dynamic validation`: profile, parameters, business invariant and result;
- `Closure decision`: `CLOSED_FOR_VALIDATION` or `OPEN_FOR_NEXT_BATCH` with the
  next trigger.

For native NDNSF-DI behavior, the production assertion, fixture/driver and
oracle remain C++; Python may launch processes and sample external resources.
`STATIC_PASS` releases the next dependent implementation or validation step;
it never upgrades the experiment result without the required runtime markers.
