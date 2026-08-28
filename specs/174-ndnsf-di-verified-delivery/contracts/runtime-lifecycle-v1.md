# Runtime Lifecycle Contract v1

## Scope

Defines the application-visible NDNSF-DI REQUEST-to-RESPONSE lifecycle and the authority transition from Provider offer to Provider execution. Existing native and Python V3 owners implement this contract; this document does not authorize a parallel coordinator.

## Required Event Order

```text
REQUEST_CREATED
REQUEST_PUBLISHED
ACK_COLLECTION_OPENED
ACK_ACCEPTED* / ACK_REJECTED*
ACK_CLOSED
PLACEMENT_PROPOSED
PLAN_VALIDATION_STARTED
PLAN_COMMITTED
SELECTION_PUBLISHED{one per selected Provider}
SELECTION_ACCEPTED{per Provider}
ROLE_PREPARATION_STARTED{per Provider}
ROLE_READY{dependency-local, per Provider}
ROLE_EXECUTION_STARTED{per Provider}
ROLE_OUTPUT_PUBLISHED{per planned output}
FINAL_RESPONSE_PUBLISHED
FINAL_RESPONSE_ACCEPTED
```

Events for concurrent Providers may interleave after `PLAN_COMMITTED`, but the following partial order is mandatory:

- every accepted ACK precedes `ACK_CLOSED`;
- `ACK_CLOSED` precedes placement;
- validation and commit precede every Selection;
- a Provider's valid Selection precedes its preparation;
- exact grant/capability and admission precede its model execution;
- all sealed dependencies precede its `ROLE_EXECUTION_STARTED`;
- the sealed final-result dependency precedes final response;
- no late event from a fenced identity can advance the active attempt.

## Request Contract

The application supplies model/task identity, input, deadline, policy, and acceptance intent. It does not supply the request-specific Provider list, role map, or final split. Existing API compatibility layers may accept legacy inputs only if the production default route still satisfies this rule and the compatibility path cannot silently become Spec174 evidence.

## ACK Closure Contract

1. Verify request/attempt binding, Provider identity/epoch, signature, permission/token, expiry, and replay status.
2. Normalize/deduplicate accepted offers deterministically.
3. Close exactly once by declared deadline, explicit sufficient-offer policy, cancellation, or fatal validation state.
4. Canonically serialize the closure and compute/sign `ackClosureDigest`.
5. Late ACKs are observable but cannot mutate the closure or a plan derived from it.

## Placement Contract

`PreSplitFirstStrategy` receives immutable sanitized values only:

- ACK closure and offer snapshots;
- canonical model graph/artifact/split-candidate snapshots;
- policy and deadline snapshot;
- resource and bounded cache/residency evidence.

It returns a proposal without I/O, keys, device/runtime handles, leases, callbacks, or mutation authority. Repeated calls with identical canonical inputs return the same proposal and explanation.

The trusted plan core validates:

- input digests match the active request/attempt/closure/policy;
- graph coverage and legal cuts;
- every role and dependency is defined and reachable;
- Provider/role mapping is bijective;
- every tensor group has complete ranks and an explicit schedule;
- capacity uses a complete peak vector and current compatible runtime evidence;
- final result and security/recovery contracts are complete;
- plan deadlines do not exceed the request deadline.

Only then may it commit one canonical plan digest.

## Selection Contract

For each selected Provider, project only:

- active request, attempt, closure, policy, and plan identities;
- exactly one complete local role/rank;
- exact artifact/assembly requirements;
- exact incoming/outgoing dependencies;
- group/rank schedule if relevant;
- local resource requirement and bounds;
- Provider-specific grant/token/security bindings.

The Provider rejects Selection before preparation if any identity, signature, token, expiry, role ownership, artifact, dataflow, group membership, runtime compatibility, or resource constraint does not exactly match. An ACK is never sufficient authority to execute.

## Provider Readiness Contract

A Provider may prepare after valid Selection. A role becomes runnable when all three are true:

1. local canonical artifacts are fetched, assembled, and graph-validated;
2. fresh local admission plus protected-runtime grant/capability checks pass;
3. every sealed input dependency is verified and ready.

No second global commit or all-Provider readiness barrier exists. An early pipeline role may execute while a downstream Provider is still assembling, provided the plan and all local preconditions are satisfied.

## Completion Contract

The final response is accepted only when:

- it matches the active request/attempt/plan/result contract;
- it is produced by the sealed final owner after all required dependencies;
- normal NDNSF signature, permission, token, replay, and freshness checks pass;
- content/oracle verification appropriate to the workload passes;
- no accepted response already exists for a single-response contract.

A local stage output, rank partial, one generated token, “model loaded,” or successful ORT invocation without the full result is not completion.

## Recovery And Fencing

| Condition | Same attempt permitted? | Required action |
|---|---:|---|
| lost Interest/Data segment | yes | retry same exact name within edge bounds |
| duplicate/reordered valid Data | yes | deduplicate/reassemble once |
| corrupt/wrong/stale Data | no acceptance | reject; retry same expected name or fail bound |
| Provider local transient before execution | only if contract permits and deadline remains | retry local preparation without rebinding role |
| Provider cannot execute after Selection | no silent substitution | fail attempt or create new plan/attempt |
| missing tensor rank | no world-size shrink | fail group epoch; replan if policy permits |
| cancellation | no | fence attempt/plan/generation/group, release and zeroize |
| Provider restart | old epoch invalid | reject old work; replan or restart with new authority |
| late old-plan output | default reject | adopt only with explicit identical-authority/result proof |

## Mandatory Negative Cases

- ACK after closure;
- proposal based on a different closure/policy/model digest;
- duplicate Provider assignment or missing role;
- one logical role split across Providers;
- Selection before commit or Selection for wrong Provider/role;
- Provider executes from ACK alone;
- role executes before dependencies or protected authorization;
- late response from cancelled/superseded attempt;
- duplicate final response under single-result contract;
- indefinite wait after no-progress/hard deadline.

## Observability

Every event records bounded request/attempt/plan/role/group identities, monotonic time, outcome, and first-failure code. Logs never contain secrets, plaintext tensors, unwrapped keys, full authorization material, or mutable runtime handles.
