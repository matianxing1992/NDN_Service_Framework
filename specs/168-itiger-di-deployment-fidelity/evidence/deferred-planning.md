# T005 Deferred Planning Evidence

**Recorded:** 2026-08-03  
**Scope:** model-first application API and post-ACK graph-aware planning  
**Focused verdict:** PASS  
**Real MiniNDN verdict:** BLOCKED by the already-retained Gate B admission
manifest; no deployment claim and no TigerCluster submission were made.

## Accepted lifecycle

The default application call is now:

```python
application.request(
    model=ModelRef(
        name="Qwen/Qwen3-0.6B",
        revision="<immutable revision>",
        content_digest="sha256:<weights>",
        tokenizer_digest="sha256:<tokenizer>"),
    input=GenerationInput(prompt="..."),
    generation=GenerationConfig(max_new_tokens=64),
    strategy=my_strategy,
)
```

Normal `app.yaml` supplies identity, Controller, group, service, trust schema,
and deadlines. It rejects roles, dependencies, shards, deployment identifiers,
artifact assignments, and other preplanned fields. The application publishes one
Request before model graph inspection or partitioning. Only after the immutable
`ACK_CLOSED` snapshot does NDNSF-DI inspect the adapter dependency graph,
enumerate bounded candidates, invoke the external strategy, prepare/publish the
selected artifacts, seal the plan, and call generic NDNSF `commit_plan()` for the
final Selection.

The old deployment-object path is explicit as `request_preplanned()`. A single
positional argument to `request()` remains a deprecated, counted shim. All
tracked production/example callers were migrated; the only remaining positional
call is the regression that proves the warning and counter. The shim deletion
gate is: zero tracked non-test callers plus an operator-observed aggregate
`preplanned_compatibility_uses == 0` for one announced deprecation window.

## Binding and negative-case matrix

| Invariant | Enforcement | Executable evidence |
|---|---|---|
| Immutable model identity | Public requests require name, immutable revision, weights digest, and tokenizer/semantics digest | `test_public_model_request_requires_immutable_revision`; `test_model_identity_uses_name_revision_content_and_tokenizer_hash` |
| No application-supplied deployment plan | `ApplicationRuntimeConfig` rejects preplanned fields | `test_normal_application_config_rejects_deployment_plan_fields` |
| Request precedes planning | Graph inspection and split enumeration happen after `ACK_CLOSED` | `test_request_is_published_before_graph_and_split_planning` |
| ACK request binding | Closure and every candidate must match the invocation request ID before graph or strategy work | `test_wrong_request_ack_snapshot_is_rejected_before_planning` |
| Late ACK immutability | A conflicting second closure is rejected; the first candidate tuple and telemetry are immutable | `test_non_di_deferred_request_closes_once_then_commits_idempotently` |
| Graph-aware external strategy | Missing dependency graph fails before the strategy callback; the strategy receives immutable graph/candidate/provider views | `test_external_strategy_is_not_called_without_dependency_graph`; `test_model_task_first_request_plans_after_signed_ack_snapshot` |
| Feasible assignment | Candidate coverage, exact role coverage, Provider capacity, aggregate GPU use, strategy time budget, and deterministic replay fail closed | `test_assignment_is_candidate_bounded_and_aggregate_gpu_safe`; `test_time_budget_and_deterministic_replay_are_enforced`; `test_unknown_runtime_bound_fails_closed` |
| Full plan binding | The sealed plan binds ACK digest, model/graph placement input, strategy name/version/state digest, decision digest, selected split, Provider offer/boot epoch/resource sequence, artifacts, and dependencies | `test_model_task_first_request_plans_after_signed_ack_snapshot` |
| No mutable commit | Dataclass fields and nested mappings are immutable; idempotent same commit is allowed, conflicting second commit is rejected | `test_non_di_deferred_request_closes_once_then_commits_idempotently`; sealed-plan mutation assertions in the automatic collaboration suite |
| Explicit compatibility | `request_preplanned()` is direct; positional shim warns and increments its counter; `deployment=` is rejected | `test_preplanned_path_is_explicit_and_positional_shim_is_counted` |

The adapter base already exposes immutable model description, graph inspection,
split enumeration, task encoding/decoding, and state contracts. T005 therefore
did not add a second LLM-specific adapter interface or move inference semantics
into generic NDNSF.

## Verification

The focused and impacted Python run passed **102/102 tests in 12 suites**:

- Spec 168 deferred-planning contract: 7
- automatic collaboration planning: 14
- pre-split strategy: 7
- placement strategy and external policy boundary: 11
- model-family adapter: 3
- application SDK compatibility/public API/user journey/catalog/handle: 45
- selection/dataflow regression: 15

`py_compile`, public API fixture JSON validation, and `git diff --check` also
passed. These are contract/focused results, not deployment evidence.

Gate B remains intentionally blocked because the retained Spec 165 run used
shared-filesystem artifact injection, test-only state, a fixed settle delay,
per-token requests, and no independent Repository/lifecycle manifest. T006 must
replace the global ReadySet barrier with plan-bound per-role data-driven
eligibility, and T007 must make real Provider fetch/load/warmup/generation
truthful before the unchanged prepared model is admitted to real MiniNDN.
