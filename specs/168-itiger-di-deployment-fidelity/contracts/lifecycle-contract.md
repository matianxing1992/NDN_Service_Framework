# NDNSF-DI Invocation Lifecycle Contract

## Public application surface

The application does not predeclare deployment roles or pre-split paths:

```yaml
# app.yaml: identities and runtime connectivity, not a deployment plan
application:
  identity: /research/ndnsf-di/user
controller: /research/ndnsf-di/controller
service: /Inference/Generate
ack_timeout_ms: 500
hard_deadline_ms: 900000
progress_idle_ms: 30000
```

```python
application = InferenceApplication.from_config(
    "app.yaml", state_root="./state/user"
)
handle = application.request(
    model=ModelRef(
        name="Qwen/Qwen3-0.6B",
        revision="<immutable-revision>",
        content_digest="sha256:<model-content>",
        tokenizer_digest="sha256:<tokenizer>",
    ),
    input=GenerationInput(prompt="..."),
    generation=GenerationConfig(max_new_tokens=64, do_sample=False),
    strategy=PreSplitFirstStrategy(),
)
result = handle.result()
```

`request()` is one durable invocation. It owns Request, ACK closure, planning,
final Selection, preparation, all token steps, and the terminal Response.

`app.yaml` configures identity, controller/service routing, and deadline policy;
it contains no roles, dependencies, shard paths, or deployment revision.

The old deployment-first API is available only as
`application.request_preplanned(deployment=..., input=...)`. During migration,
the old positional `application.request(deployment, ...)` form may dispatch to
that method with a deprecation warning and a compatibility-use counter. It is
never selected automatically, does not satisfy Spec 168 acceptance, and is
removed after all tracked repository callers use either the model-first API or
the explicitly named compatibility method.

## External strategy surface

```python
class PlacementStrategy(ABC):
    def plan(
        self,
        *,
        invocation: InvocationIdentity,
        model: ModelIdentity,
        graph: DependencyGraph,
        candidates: Sequence[ProviderCapabilitySnapshot],
        artifact_catalog: ArtifactCatalogView,
        constraints: PlacementConstraints,
    ) -> CollaborationPlan:
        ...
```

The graph is adapter-produced and explicit. The strategy may reuse compatible
published partitions or generate a new graph cut after ACK closure. The default
`PreSplitFirstStrategy` orders feasible placements by compatible GPU residency,
then host-memory, disk, published artifact, and finally new partition/publication
cost, subject to capacity and dependency constraints.

## Normative event order

```text
REQUEST_CREATED
  -> REQUEST_PUBLISHED
  -> ACK_ACCEPTED* / ACK_REJECTED*
  -> ACK_CLOSED
  -> GRAPH_INSPECTED
  -> PLAN_VALIDATED
  -> ARTIFACT_PUBLISHED*       # only missing partition identities
  -> PLAN_COMMITTED / FINAL_SELECTION
  -> ROLE_ASSIGNED+
  -> [per role independently]
       ARTIFACT_FETCHED*
       -> VERIFIED_DISK
       -> HOST_RESIDENT
       -> GPU_RESIDENT
       -> LOCAL_READY
       -> DEPENDENCY_INPUT_ACCEPTED*
       -> STAGE_EXECUTING
       -> STAGE_OUTPUT_PUBLISHED
       -> STAGE_COMPLETED
  -> TOKEN_RECORDED*
  -> RESPONSE_PUBLISHED | FAILURE_PUBLISHED | CANCELED | EXPIRED
  -> CLEANUP_RECORDED
```

`ARTIFACT_PUBLISHED` may occur between validated planning and final Selection so
the committed plan refers only to immutable, fetchable artifact identities. It
does not mean Providers prepare before Selection.

## Bounded Selection fanout

`commit_plan` is one logical Selection transition. For a multi-Provider
collaboration, Core projects the committed plan into one authenticated
provider-specific Selection message per selected Provider. Every projection
uses the original `request_id`, attempt, plan binding, Provider token proof, and
exact opaque assignment; no projection reopens ACK collection or creates a new
wire Request. This is message fanout inside one durable invocation, not retry or
multiple invocation attempts.

The projection boundary is mandatory because the protected wire size includes
assignment bytes plus hybrid-envelope, wrapped-key, TLV, signature, and SVS
overhead. An all-assignment compact message may be used only outside
collaboration when its complete protected representation is bounded. Missing,
oversize, duplicate, wrong-Provider, or wrong-plan projections fail closed and
remain visible through per-Provider Selection status.

## Binding invariants

Every accepted event after Request creation must match:

```text
request_id
attempt_epoch
requester/service security domain
plan_digest (after commit)
provider identity + boot epoch + role (role-scoped events)
model/artifact/dependency identity (when applicable)
```

Late, duplicate, stale-attempt, wrong-plan, wrong-provider, or unauthenticated
events cannot mutate live state or refresh progress.

## Data-driven execution

Plan commitment is role authorization. A role runs when and only when:

```text
valid committed assignment
+ local adapter-confirmed preparation
+ all direct predecessor inputs
+ nonterminal request and valid deadline
```

Stage 0 has an empty predecessor set. Therefore local preparation immediately
makes it runnable. No `all_roles_ready`, Provider settle window, or global
`ExecutionActivateMessage` is part of this predicate.

## Execution-policy migration

Every plan declares exactly one execution policy:

- `DATA_DRIVEN_V2` is the default dynamic NDNSF-DI policy. The committed signed
  plan authorizes each assignment; local preparation and direct input arrival
  trigger execution. It never waits for or accepts a global activation message.
- `LEGACY_READY_SET_V1` preserves the old all-member activation path only for an
  explicitly requested preplanned compatibility invocation. It is not an
  automatic fallback and cannot satisfy Spec 168 acceptance.

Providers advertise supported execution policies in ACKs. Planning must choose
one policy supported by every selected Provider and bind it into the plan digest.
A mixed-policy plan, a Provider that lacks the committed policy, or a V1
activation sent to a V2 assignment fails closed before execution. The policy
cannot change after plan commit. Operational rollback means admitting a new,
explicit V1 compatibility invocation with a new identity; it never mutates or
silently downgrades a committed V2 request. Legacy-use counters are retained so
the compatibility path can be reviewed and removed separately.

## Token and response semantics

- The prompt/token input crosses the collaboration boundary once.
- Token iteration stays inside the committed distributed execution session.
- Token records are ordered by generation index and bound to the invocation.
- The terminal Response contains the complete ordered answer and terminal reason
  (`EOS`, `MAX_TOKENS`, or a defined application stop).
- `wire_request_count == 1` and `token_request_count == 0` for the default path.

## Terminal semantics

Exactly one of `RESPONSE`, `FAILURE`, `CANCELED`, or `EXPIRED` wins. The first
authenticated terminal transition is durable and idempotent. Later terminal or
stage events are retained as rejected late evidence but cannot alter the result.

## Security

Normal NDNSF permission distribution, NAC-ABE service/permission attributes,
one-time UserToken and ProviderToken checks, replay protection, plan signatures,
Provider permissions, and authenticated terminal Response remain mandatory.
Skipping the global readiness barrier does not weaken these checks.
