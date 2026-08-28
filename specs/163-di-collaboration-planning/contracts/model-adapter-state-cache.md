# Model-Family Adapter and Inference-State Contract

## 1. Purpose and boundary

This contract makes the Spec 163 application path independent of any one model
family and defines KV cache as one reusable inference-state profile rather than
as a framework-wide LLM primitive.

Base NDNSF carries authenticated opaque request, ACK, plan, Selection, object,
status, and Response bytes. NDNSF-DI owns every model, task, graph, split,
runner, state, KV, and cache semantic. No adapter may add a model-specific NDNSF
wire message.

## 2. Public application request

The target application surface is one durable invocation:

```python
handle = app.request(
    model=ModelRef(
        model_name="Qwen/Qwen3-0.6B",
        content_digest="sha256:<signed-manifest-digest>",
        semantics_digest="sha256:<tokenizer-config-preprocess-digest>",
    ),
    task=InferenceTaskRef("text-generation"),
    input=ApplicationInput.text("Explain named-data networking."),
    options=GenerationOptions(max_new_tokens=64, temperature=0.0),
    timeout=120.0,
)
```

The same function accepts, for example, object-detection image input, speech
audio input, diffusion conditioning, or an opaque-container schema. The
framework does not define `prompt`, `token`, `logits`, `sampling`, or `KV`
fields. `ApplicationInput`, result, and options are validated by the selected
task adapter and carried as bounded schema-identified bytes.

`app.yaml` is operator configuration: local identity and trust anchors, state
root, repository prefix, adapter allowlist, default planning strategy, cache
budgets, and policy. It does not contain a per-request model, Provider list,
roles, deployment, secret key, or precomputed split.

## 3. Composed `ModelFamilyAdapter`

`ModelFamilyAdapter` is a composition root, not one privileged god object:

```python
class ModelFamilyAdapter(Protocol):
    descriptor: ModelAdapterDescriptor
    graph: GraphAdapter
    splitter: ModelSplitter
    task_io: TaskAdapter
    state: StateAdapter
    runner: RunnerAdapter
```

| Port | Pure/data responsibility | Forbidden authority |
|---|---|---|
| `GraphAdapter` | Verify model identity and emit canonical `ModelGraphSnapshot` | Network, repository mutation, device load |
| `ModelSplitter` | Enumerate bounded graph-valid `SplitCandidate` values | Materialization, publication, Selection |
| `TaskAdapter` | Declare task, input/options/result schemas and encode/decode | Provider choice or runtime execution |
| `StateAdapter` | Declare state classes, exact key material, cost and lifecycle | Reading other tenants or retaining state by itself |
| `RunnerAdapter` | Create the separately trusted executable runner after Selection | Changing the accepted plan or state authorization |

`ModelAdapterDescriptor` binds:

- stable adapter name and semantic version;
- implementation/state digest and ABI;
- supported model formats, tasks, backends, precisions, and state profiles;
- input, options, output, graph, split, and state schema digests;
- whether graphs are inspectable, splittable, and deterministically analyzable.

Adapter resolution is allowlisted and digest-pinned. Adapter output is
untrusted data and passes common graph, size, schema, resource, and deadline
validation. Executable runners remain explicit host-TCB members.

An adapter that cannot safely reveal internal dependencies emits an atomic
single-node graph with no legal cut edge. It can still run on one eligible
Provider; it must not invent a split.

## 4. Inference-state classes

Every task adapter declares zero or more `InferenceStateContract` values:

| Class | Typical example | Default terminal rule | Cross-request reuse |
|---|---|---|---|
| `STATELESS` | Stateless image classifier | No mutable state | No |
| `REQUEST_SCOPED` | Activations, scratch workspace, ordinary KV | Destroy/release | No |
| `SESSION_SCOPED` | Explicit conversational session state | Retain only for the authorized session and TTL | Same authorized session only |
| `EXACT_PREFIX_REUSABLE` | LLM prefix KV | Retain only under explicit policy and identity | Exact compatible security domain only |
| `CUSTOM_ADAPTER_MANAGED` | Diffusion or streaming state with a defined profile | Adapter contract decides, fail closed if absent | Only as explicitly declared |

Each contract states identity inputs, owner, role/layer scope, byte estimator,
device and host budgets, allowed tiers, access domain, confidentiality,
retention TTL, eviction policy, boot/cache epoch rules, pin/reservation
semantics, migration support, revalidation, and cleanup.

Immutable model/runtime artifacts are governed by the pre-split catalog.
Mutable inference state is governed here. A semantic response cache is a third,
separate concern and is not introduced by Spec 163.

## 5. Exact prefix-KV profile

### 5.1 Canonical state identity

The adapter canonicalizes a private `PrefixKvIdentity` and exposes only
`state_key_digest = SHA-256(canonical identity bytes)`. The identity binds:

- exact model content and semantics digests;
- adapter identity/version/digest and runner ABI/digest;
- split/manifest digest, role identity, and owned layer range;
- exact prefix-token digest, token count, position IDs, context/window
  parameters, attention mode, and any rope/scaling convention;
- precision, tensor shape/layout, KV implementation/version, and device/backend
  compatibility;
- requester tenant/security domain and authorized session when applicable;
- Provider identity, boot epoch, cache epoch, entry generation, and expiry.

Similarity, matching prompt text, model name alone, moving revision, or equal
token count is never an acceptable cache key.

### 5.2 ACK evidence

A signed `DIProviderOfferV2` may include a bounded list or digest-indexed summary
of `ReusableStateOffer` values:

```text
state_key_digest
state_profile = EXACT_PREFIX_KV_V1
role_or_layer_range
bytes
tier
access_domain_digest
provider_boot_epoch
cache_epoch
entry_generation
expires_at
pin_capability
migration_capability
```

The offer never carries prompt text, token IDs, plaintext state, decryption
keys, raw tenant identifiers, unrestricted cache membership, or mutable cache
handles. The coordinator verifies signature, request/profile binding,
freshness, bounds, and access policy before producing the narrower
`ProviderPlanningView`.

ACK evidence is a time-varying offer, not proof that the state will remain
available. It grants neither a cache pin nor execution authority.

### 5.3 Planning, commit, and Selection

After `ACK_CLOSED`, the strategy may propose a `StateReuseBinding` containing:

```text
provider / role / layer range
state_key_digest / profile / entry generation
expected bytes and avoided work
required access-domain and epoch evidence
required pin or reservation
fallback = CLEAN_COMPUTE | REPLAN
```

Trusted NDNSF-DI validation recomputes compatibility and resource accounting.
The accepted binding becomes part of the plan digest passed through generic
`commit_plan` and the opaque per-Provider final Selection assignment. Selection
acceptance atomically installs the role, state authorization, budget, and
pin/reservation through the generic Selection transaction participant.

Immediately before use, the Provider revalidates digest, authorization,
generation, boot/cache epoch, expiry, layout, layer coverage, byte bounds, and
pin. Failure follows the sealed fallback; it never silently uses approximate or
uncovered state.

For pipeline parallelism, each stage owns KV only for its assigned layer range.
The baseline requires complete exact compatible coverage for every range marked
reused. A partial stage hit may be represented explicitly as a mixed
reuse/recompute plan, but must not be reported as a whole-request hit.

### 5.4 Execution and terminal convergence

Execution records hit/miss reason, covered prefix tokens and layer range,
avoided prefill work, bytes, tier, pin timing, fallback, and eviction. It never
records plaintext prompts or tokens in public evidence.

At terminal convergence:

- request-scoped state is destroyed;
- reusable state is retained only when the state contract, requester
  authorization, security domain, TTL, and cache budget all permit it;
- release removes invocation grants and pins;
- eviction increments the relevant cache generation/epoch and updates later ACK
  evidence;
- Provider restart invalidates GPU/RAM residency; persistent encrypted state
  must be reverified before advertisement or use.

Cross-tenant reuse is denied by default even when content-derived digests match.

## 6. Optional migration

Provider-local reuse is the baseline. Reusable state is not published under the
public immutable model-artifact namespace.

An adapter may declare an optional `StateMigrationContract` only when the state
format is portable. It must bind source and recipient, model/state identity,
lineage, role/layer range, format, encryption suite, nonce discipline, segment
digests, maximum bytes, deadline, replay epoch, source and destination resource
reservations, and cleanup. Transport uses existing authenticated collaboration
objects or an access-controlled NDNSF-DI service over generic NDNSF carriage.

Migration failure, excessive cost, expiry, incompatibility, or missing
authorization falls back to clean computation or a fresh attempt. Migration is
not required by the baseline MiniNDN gate.

## 7. Strategy cost and safety rules

The placement strategy may compare:

```text
clean_prefill_cost
local_state_revalidation_cost
state_transfer_cost (only if migration is allowed)
model_shard_fetch/load cost
network RTT/bandwidth
queue delay
GPU/RAM capacity including state bytes
deadline and uncertainty margin
```

A cache hit does not override role feasibility, model-shard identity, resource
exclusion, deadline, or security. The strategy sees immutable sanitized values
and cannot read, mutate, pin, migrate, or delete state.

## 8. Required verification

1. LLM, object-detection, and opaque-container fixtures use the same public
   request and generic collaboration carrier.
2. The opaque-container adapter emits a one-node graph and is never split.
3. Static ownership checks find no LLM/KV parsing in base NDNSF Core.
4. Exact local prefix-KV reuse preserves the frozen reference result and avoids
   only the covered prefill work.
5. Every single identity component mismatch, tenant mismatch, expiry, eviction,
   restart, failed pin, and tampered offer rejects reuse.
6. Request-scoped state is absent after terminal convergence.
7. Duplicate/reordered ACK, commit, Selection, status, cancel, and eviction
   events preserve at-most-once role admission and first-terminal-wins.
8. Migration-disabled and migration-failure cases cleanly recompute; optional
   migration tests additionally prove authorization, encryption, integrity,
   replay fencing, bounded resources, and cleanup.

