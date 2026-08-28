# Contract: Provider Assignment Policy

This contract specializes `contracts/optimization-extension.md` for unified
ordinary one-role and multi-role Provider assignment. It is implemented through
the public SDK, not by importing Runtime v1 internals.

## Input

Policy receives:

- immutable `PlanCandidateSet` produced by `PartitionPlanner`, or one exact
  operator-selected plan;
- bounded authorized `ModelVariantCandidateSet` or exact-model singleton lineage;
- immutable eligible `CandidateSnapshot` produced by Core;
- bounded compatible `ExecutionTargetCandidateSet` entries for each applicable
  plan-role-Provider alternative;
- one synthetic role plus requested multiplicity for ordinary service requests,
  or explicit plan roles/multiplicities for collaboration requests;
- shared immutable `OptimizationObjective` and the assignment least-input
  projection of the current `EngineSnapshot`;
- cache affinity and other advisory facts, never a pre-authorized Provider;
- immutable `DecisionBudget` and replay seed.

Policy does not receive authority to modify ACK success, eligibility, leases,
telemetry age, attempt epoch, plan identity, token/permission state or security
state.

## Output

Policy returns one `ProviderAssignmentDecision` containing:

- policy name, version and digest;
- model-variant, candidate-set, objective and snapshot identity/digest;
- selected model-variant identity and selected plan identity bound to it;
- complete Provider assignments, selected role-target identities and requested
  multiplicity;
- deterministic score/explanation for every considered eligible candidate;
- ranked alternatives and policy-specific rejections;
- decision timestamp and stable decision identity;
- shared optimization evidence and explicit fallback identity.

## Core application rules

Core MUST:

1. reject a decision for a different/stale model/plan candidate set or snapshot;
2. reject missing, duplicate, unknown or multiplicity-invalid assignments;
3. revalidate ACK membership, selected Provider availability,
   fragment/resource feasibility, token/permission binding and leases;
4. verify that the selected variant is authorized, the plan is bound to it and
   every selected target is a supplied compatible candidate for its role and
   Provider, then create the accepted `AssignmentContext` itself;
5. retain existing bounded deadline/recovery rules;
6. record typed rejection evidence;
7. apply no assignment until model/plan/assignment/target state is atomically
   committed in the current `ValidatedExecutionIntent`.

## Required reference policies

- `FixedProviderAssignmentPolicy`: deterministically chooses
  application-specified bindings and fails if they are not eligible.
- `FirstRespondingProviderAssignmentPolicy`, `RandomProviderAssignmentPolicy`
  and `AllProviderAssignmentPolicy`: preserve ordinary NDNSF-DI one-role
  selection behavior.
- `CostProviderAssignmentPolicy`: preserves pre-separation Runtime v1 and native
  multi-role scoring with explicit versioned weights; it is a compatibility
  policy, not a claim of global optimality.
- `CacheAwareProviderAssignmentPolicy`: consumes cache affinity from
  `CachePolicy` together with all other eligible facts; cache policy itself does
  not select the compute Provider.

No assignment policy is a network service or authority.
