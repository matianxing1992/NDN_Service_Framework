# T006 Per-Role Data-Driven Execution Evidence

**Date**: 2026-08-03  
**Scope**: local contract/runtime evidence only  
**Deployment claim**: none; MiniNDN, exact-container, and TigerCluster gates remain closed until T007 completes truthful Provider preparation and generation.

## Causal baseline

The retained V1 compatibility path proves the behavior being removed from the
default path. `ReadySetCoordinator.activate()` rejects an incomplete exact
member set, and `ProviderActivationGate` accepts only a signed activation bound
to the request, attempt, Selection digest, DeploymentPlan digest, Provider boot
epoch, deadline, and complete membership. Therefore Stage 0 cannot start on the
V1 path until every selected role is ready.

This baseline is preserved deliberately; it is no longer the default execution
authority.

## V2 authorization invariant

For each selected role `r`, execution eligibility is:

```text
eligible(r) = committedPlan(r)
           && localPreparationReady(r)
           && everyDirectPredecessorInputVerified(r)
           && !failed(r)
           && !alreadyStarted(r)
```

Consequences:

- a source role starts immediately after its own preparation because its direct
  predecessor set is empty;
- a downstream role starts only after every scope listed in its exact committed
  `DIRoleAssignmentV2.required_input_scopes` has been fetched successfully;
- an unknown scope, an altered assignment, a different request/attempt/plan, or
  an uncommitted assignment cannot create readiness;
- duplicate dependency notification is idempotent and cannot start a role twice;
- no all-Provider `ReadySet` or `ExecutionActivateMessage` is consulted by the
  `DATA_DRIVEN_V2` path.

`ProviderRuntimeContext` reports dependency readiness only after the synchronous
fetch returns data or the asynchronous large-object future resolves successfully.
The Qwen/native runners still consume dependency edges through the normal
authenticated Collaboration data path.

## Explicit execution-policy boundary

| Layer | `DATA_DRIVEN_V2` | `LEGACY_READY_SET_V1` | Mixed/unknown |
|---|---|---|---|
| Positive ACK offer | default advertised capability | may be advertised only explicitly | rejected |
| Deferred Selection | required and sealed in every `DISelectionAssignmentV2` | rejected by the V2 Selection schema | rejected |
| Sealed Collaboration plan | required and included in the plan digest | not accepted | rejected |
| Preplanned `DeploymentPlan` | cannot enter `ReadySetCoordinator` or `ProviderActivationGate` | explicit compatibility default and counted on first activation | rejected |
| Native plan/Provider | plan and Provider config must match; legacy flags must be off | plan, config, assignment field, activation flag, and readiness-barrier flag must all agree | rejected before execution |

There is no automatic policy fallback. A rollback from a V2 invocation creates a
new V1 invocation with a new request ID, Selection digest, deadline, and fresh
Provider resource assignments. It does not mutate or downgrade the in-flight V2
request. Legacy activation acceptance is counted once; duplicates remain
idempotent.

## Security and lifecycle bindings retained

- Core opaque Selection commit remains the only transition that creates role
  gates and launches preparation.
- The signed ACK offer is checked for exact request, attempt, Provider, boot
  epoch, resource sequence, expiry, accepted deadline, role coverage, and
  execution-policy support before a resource hold can commit.
- Assignment bytes bind invocation, request, attempt, plan, dependency graph,
  Provider, boot epoch, roles, artifacts, resource sequence, and generation.
- Conflicting commit projections, altered assignment bytes, stale offers,
  unselected roles, foreign scopes, and policy mismatches fail closed.
- Existing signature, replay, expiration, boot-epoch, and exact ReadySet checks
  remain intact on the isolated V1 path.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference python3 tests/python/test_ndnsf_di_execution_consistency.py
  PASS: 7/7

PYTHONPATH=NDNSF-DistributedInference python3 tests/python/test_ndnsf_di_selection_dataflow.py
  PASS: 20/20

PYTHONPATH=NDNSF-DistributedInference python3 tests/python/test_ndnsf_di_automatic_collaboration_plan.py
  PASS: 14/14

Impacted Python suites (execution consistency, Selection dataflow, automatic
planning, placement, Spec 129 compatibility, Qwen generation, public API, and
user journey):
  PASS: 79/79

PATH=/usr/bin:/bin ./waf build --targets=unit-tests -j4
  PASS: C++ unit-test binary rebuilt with the system linker

build/unit-tests --run_test=NativeProviderExecutionPolicyRejectsMixedFallback
build/unit-tests --run_test=NativeExecutionPlanLoadsFromGeneratedJsonShape
build/unit-tests --run_test=NativeExecutionPlanRejectsAutomaticPolicyFallback
build/unit-tests --run_test=Spec111NativePlanSessionAndAttemptDefaultsAreCharacterized
  PASS: 4/4 focused C++ tests

PATH=/usr/bin:/bin ./waf configure --with-tests --with-examples
PATH=/usr/bin:/bin ./waf build --targets=di-native-provider -j4
  PASS: build/examples/di-native-provider
```

The first unrestricted `./waf build` attempt was not accepted as evidence: it
was interrupted by the unrelated `App_SvsLatency` target using the Linuxbrew
linker with system Boost/libcrypto. Reconfiguration with `PATH=/usr/bin:/bin`
proved Boost 1.71 linkage and enabled the intended unit-test/example targets.

## Gate status

T006 code/contract gate: **PASS** after the commands above and the final native
Provider executable build.  
Spec 168 deployment Gate B: **BLOCK** pending T007; no remote submission is
authorized by this evidence alone.
