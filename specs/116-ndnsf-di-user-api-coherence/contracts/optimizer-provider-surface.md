# Contract: Optimizer and Provider Extension Surface

## Policy Families

The accepted decision graph retains ten policy seams:

1. model variant
2. partition
3. deployment topology
4. provider assignment
5. scheduling
6. admission
7. execution tuning/resource allocation
8. cache
9. recovery
10. execution target/backend

These seams may share internal evidence utilities, but every public protocol
uses its own named request and result dataclasses. Required inputs, units,
candidates, upstream decisions, and outputs are concrete fields; neither aliases
nor accessors over a shared `metadata` dictionary satisfy this contract.

## Partial Suite Composition

```python
suite = (
    OptimizationSuite.defaults()
    .replace(provider_assignment=MyProviderPolicy(config))
    .build(name="lab-policy", version="1")
)
```

The builder validates policy protocol/type/version/config and calculates a
canonical suite descriptor/digest from ordered public descriptors. Omitted
policies retain versioned defaults and are identified as defaults in evidence.

## Decision Contract

Every policy invocation's dedicated request declares the applicable subset of:

- immutable engine snapshot and state epoch;
- typed objective and budget;
- bounded typed candidates;
- upstream decision outputs explicitly required by that seam;
- cancellation/deadline context.

Every dedicated result contains its seam-specific typed decision plus evidence
binding policy kind,
implementation/version, suite identity, state epoch/digest, candidate set,
decision digest, and default/custom status. The engine validates the result
before any downstream decision or provider action.

Provider-assignment input may include validated `ProviderDeploymentOffer`
availability. A custom/default policy may rank exact READY offers ahead of
NEEDS_PREPARATION to reduce cold-start work, but the result remains merely a
selection decision. The coordinator must establish exact post-Selection
readiness for whichever valid candidate is chosen. No policy may turn ACK
metadata into an execution certificate.

## Failure Contract

Policy timeout, exception, malformed result, out-of-set candidate, inconsistent
epoch, or invalid evidence is a structured decision failure. Fallback is allowed
only when the selected local profile explicitly names a bounded fallback; it is
never an implicit import/runtime exception handler.

## RunnerAdapter Independence

`RunnerAdapter` owns model loading, input/output conversion, execution,
checkpoint/state mechanics, and backend-specific evidence. Policies select an
execution target descriptor; the runner registry resolves an installed adapter
only after execution-certificate validation.

Runner adapters do not perform provider discovery, placement, scheduling,
admission, deployment commit, or request fencing.

## Provider Role Separation

Application model authors use:

```python
provider.serve(service, runner, capabilities=capabilities)
provider.run()
```

The authenticated deployment coordinator separately constructs the advanced
`ProviderAdminPort` for stage, activate, drain, delete, readiness, progress,
checkpoint, and lifecycle receipts. It is not exposed as
`InferenceProvider.admin`. Possessing a serving facade, runner, or service
registration does not grant or reveal this administrative authority.

Provider offers and readiness use typed NDNSF-DI APP contracts. Observational
progress maps onto the generic NDNSF Collaboration status API over the existing
signed `SELECTION-STATUS` path; DI owns phase details and exact readiness while
NDNSF owns query/watch/wait mechanics. They do not introduce base NDNSF message
types or a parallel Targeted status service. Optional explicit prewarming and
request-time preparation invoke the same administrative delegates and
monotonic status state.

## Package Contract

`sdk.__all__`, `planner.__all__`, and provider-facing exports are explicit.
Contract tests reject `Any`, `Protocol`, `Enum`, dataclass helpers, modules,
internal registries, and compatibility-only aliases unless a name is explicitly
approved as public.
