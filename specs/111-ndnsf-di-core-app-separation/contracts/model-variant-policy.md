# Contract: ModelVariantPolicy

## Applicability

`ModelVariantPolicy.propose` is a public Python optimization port only for an
APP/operator-authorized `ModelAlternativeSet` containing at least two
semantically acceptable alternatives under an explicit quality contract.

If the operator supplies one exact model identity, revision, tokenizer,
precision/quantization and artifact tuple, the engine does not invoke this
policy. It records `NOT_APPLICABLE_EXACT_CONSTRAINT`, creates a singleton
candidate set, then passes the exact tuple to deployment, partition and target
compatibility validation.

## Immutable input

- complete `ModelAlternativeSet` identity/digest;
- model family/size/identity, revision and tokenizer per alternative;
- precision, quantization, artifact digests and compatible adapter identities;
- optional draft-model alternatives and compatibility declarations;
- quality metric identity, floor, provenance and exact constraints;
- least-input model/deployment/runtime portion of `EngineSnapshot`;
- shared `OptimizationObjective` and `DecisionBudget`.

Weights or unverified model files are never sent to the policy.

## Typed output

`ModelVariantCandidateSet` contains:

- one or more retained authorized alternative identities and their complete
  immutable tuples, bounded by `DecisionBudget`;
- optional draft model only when declared compatible by each alternative;
- stable rank, rejected/pruned alternatives and per-candidate explanation;
- policy/configuration/input/objective/snapshot/seed/output lineage.

## Validation

APP/model-adapter validation rejects:

- any synthesized or out-of-set model, revision, tokenizer, precision,
  quantization, artifact, adapter or draft-model identity;
- a retained candidate whose declared quality capability does not satisfy the hard
  metric/floor contract;
- a stale/mixed snapshot, expired budget or mismatched alternative-set digest;
- any attempt to replace an exact constraint or alter security/authority state.

`PartitionPlanner` may produce plans only for retained variants and binds every
plan to its variant identity. `ProviderAssignmentPolicy` selects one authorized
variant/plan/assignment tuple; it cannot synthesize a new variant.
`DeploymentPolicy` may manage only compatible lifecycle alternatives.
`ExecutionTargetPolicy` may propose only compatible runtime/device/adapter
candidates; assignment selects their bindings and cannot reinterpret the
authorized model set.

Candidate expansion and pruning are bounded and deterministic under the replay
seed. Request tokenizer, prompt-formatting, sampling, stop and decoding
semantics remain exact unless the APP explicitly authorized alternatives.

## Default and external acceptance

The named reference default is deterministic
`ndnsf.default.model-variant.exact-or-compatible-set/v1`: exact constraints
record not-applicable; otherwise it preserves the bounded compatible authorized
set under stable input ordering after hard quality filtering. It is a
compatibility/default baseline, not a claim of optimal model quality.

The external fixture must demonstrate at least three model sizes and two
precision/quantization profiles, exact-constraint preservation, out-of-set and
quality-floor rejection, seeded pruning replay and two cases where the final
model changes with plan/Provider feasibility without source edits or private
imports.
