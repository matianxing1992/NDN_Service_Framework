# Academic and Architecture Audit: Spec 163 Model-Neutral Planning and State Reuse

**Date**: 2026-07-28  
**Mode**: Pre-implementation design audit plus post-implementation evidence audit  
**Design verdict after remediation**: **PASS**  
**Post-implementation verdict**: **PASS WITH EXPLICIT NO-CUDA DEFERRAL**

## Question audited

Can NDNSF-DI make an expensive first request perform model partitioning,
NDNSF-DistributedRepo publication, Provider fetch, persistence, and GPU loading,
then make later exact-compatible requests faster by retaining and advertising
model shards in Provider caches? Can it also support multiple model families
and exact prefix KV reuse without binding base NDNSF or the public API to LLMs?

## Findings

### 1. The research direction is valid

Cold-start amortization is a coherent systems hypothesis. Immutable
model/runtime shards can outlive an inference request. Request inputs, outputs,
activations, plaintext grants, temporary decrypted data, and every mutable
state class lacking an explicit reusable contract cannot. The revised design
separates immutable model cache, request/session state, exact reusable derived
state, and semantic response caching.

### 2. Serialized model size is not a valid standalone partition rule

The example “30 GB model / 12 GB GPU” establishes only a lower bound on device
count. It does not prove that three stages fit. The reference strategy must
estimate:

```text
weights + activations + KV/workspace + transient overhead + safety margin
```

per graph-valid segment. Near-equal contiguous division is defensible only for
homogeneous graph units and equal usable GPU envelopes. Unknown bounds fail
closed. This is a baseline heuristic, not a global-optimality claim.

### 3. Cache residency is evidence, not timeless truth

GPU/RAM/disk residency cannot live in the static pre-split manifest. It is a
fresh signed `DIProviderOfferV2` fact bound to exact model/tokenizer,
manifest/artifact digest, graph range, backend, precision/runtime ABI, trust
policy, device, Provider boot epoch, cache epoch, observation time, and expiry.

An unpinned GPU claim may disappear before Selection. It can make a plan
preferable, but can make the plan feasible only if a bounded reuse pin protects
it through Selection expiry or a separately feasible reload path exists.
Selection revalidates the claim. Restart invalidates GPU/RAM residency; disk
residency requires renewed verification.

### 4. The strategy must remain pure

`PreSplitFirstStrategy` owns the policy decision:

- select an exact feasible immutable manifest, preferring safe GPU reuse; or
- derive a data-only `SplitSpecification` from the closed ACK snapshot.

It does not split files, write a repository, fetch artifacts, load devices, or
send Selection. After independent validation, trusted NDNSF-DI
`SplitMaterializer` and `DistributedArtifactPublisher` ports create
content-addressed shards and atomically activate a signed
NDNSF-DistributedRepo manifest. Final Selection is forbidden until that
publication is complete.

### 5. The lifecycle remains one inference invocation

The corrected attempt lifecycle is:

```text
begin_collaboration(DEFERRED) -> Request -> immutable ACK_CLOSED
-> reuse-or-split planning
-> optional trusted materialization and repository publication
-> commit_plan(same invocation) -> final Selection per Provider
-> exact cache revalidation or NDN fetch/verify/persist/load
-> role-local data-driven execution -> complete Response
```

There is no `PreparationCommit`, second willingness decision, second ACK round,
or all-role readiness barrier. A role starts when its own Selection, local
model readiness, and authenticated direct inputs are all satisfied.

### 6. The existing NDNSF Collaboration API is the correct normative carrier

Code inspection shows that Core already provides generic collaboration
planning, ACK selection, final Selection, `CollaborationContext` data/status,
and final Response primitives. Its present limitation is temporal: roles and
dependencies must be supplied before Request. The minimum architecture change
is therefore a generic, DI-opaque
`begin_collaboration -> ACK_CLOSED -> commit_plan` extension.

Existing roles/dependencies-before-Request calls remain `PREPLANNED`
compatibility. Spec 163 defaults to `DEFERRED`. Both modes share one state
machine and the existing Selection/Provider path. The three deferred boundaries
are local API/state transitions, not new wire messages, a preparation handshake,
or Provider acceptance.

The audit rejects placeholder roles, a coordinator-only collaboration followed
by a private DI protocol, and any Core branch that interprets model/GPU/split
semantics.

### 7. Model neutrality requires composed adapters

The current deployment-oriented request and narrow
`RunnerAdapter.supports/create_runner` seam do not express model graph,
task-I/O, state identity, or safe split behavior. The corrected design uses a
digest-pinned `ModelFamilyAdapter` composition: graph/split, task-I/O, state,
and a separately trusted runner. An opaque model may emit one unsplittable
graph node. This avoids both ONNX-only assumptions and a privileged “god
adapter.” Prompt/token/sampling/logits/KV fields remain adapter-specific and
never enter base NDNSF.

### 8. KV is reusable derived state, not a model artifact

Exact prefix KV reuse is academically defensible only when its opaque identity
binds model and semantics, adapter/runner ABI, split and layer range, exact
prefix tokens and positions, precision/layout, security domain, Provider
epochs, and expiry. ACK evidence is time-varying and grants no read authority.
The accepted reuse binding is sealed into plan and Selection, atomically
pinned/authorized, and immediately revalidated. Cross-tenant reuse is denied by
default. Provider-local reuse is the baseline; optional encrypted migration is
a separately bounded optimization. Approximate reuse and public model-repository
publication are rejected.

## Threat and failure audit

The design now fails closed for:

- stale, forged, wrong-boot, wrong-cache-epoch, or incompatible residency;
- eviction between ACK and Selection without a pin or reload fallback;
- cache poisoning, digest mismatch, revocation, corrupt/mixed repository
  segments, and partial publication;
- double-counting reusable resident bytes as both occupied and new demand;
- selected/in-flight shard eviction and cross-request leakage of request state;
- materialization or publication failure before Selection;
- fetch, verification, disk promotion, RAM/GPU load, or OOM failure after
  Selection, using bounded failure/replan policy.
- prompt/token/cache-membership disclosure, cross-tenant reuse, approximate
  prefix matching, wrong adapter/runner/split/layer/position/layout reuse,
  stale entry epoch, expiry, eviction, restart, and failed pin;
- undeclared state retention and accidental treatment of KV as a public
  immutable model artifact.

Signatures prove origin and integrity, not correct computation by a malicious
authorized Provider. Byzantine correctness remains outside the baseline claim.

## Evidence contract

The default real-model validation is MiniNDN with an exact pinned
`Qwen/Qwen3-0.6B` `ModelRef` with exact content and semantics digests:

1. cold caches: dynamic split, repository publication, NDN fetch, verification,
   persistence, load, and complete real answer;
2. exact warm reuse: five real prompts, at most 64 generated tokens, one
   unmeasured warmup, and five measured repetitions per prompt;
3. retained answers and distributions for correctness, TTFT, per-token and
   total latency, tokens/s, all preparation stages, cache tier/reason, bytes
   fetched, utilization, success, and recovery;
4. an exact warm GPU hit requires zero re-split, zero re-publication, zero
   repository model-byte fetch, and zero GPU reload;
5. cache mismatch, eviction, restart, corruption, revocation, and pin-expiry
   negative cases.
6. LLM, object-detection, and opaque one-node container adapters through the
   same public call and carrier, with zero LLM-specific Core fields.
7. request-state cleanup plus exact local prefix-KV hit and every identity,
   security-domain, epoch, expiry, eviction, restart, pin, and
   migration-disabled fallback row.

If local CUDA is unavailable, MiniNDN still validates the network, integrity,
lifecycle, split/publication, and host-cache claims, but the GPU row remains
`DEFERRED`. TigerCluster and larger-Qwen evidence require explicit separate
authorization and are not Spec 163 exit gates.

## Post-implementation audit

### Verdict

`PASS WITH EXPLICIT NO-CUDA DEFERRAL`

No unresolved CRITICAL or HIGH finding invalidates the Spec 163 architecture
or local acceptance claim. The implementation uses the existing generic NDNSF
Collaboration carrier, keeps DI semantics outside base Core, validates signed
offers and exact assignments, commits one opaque Provider transaction
crash-atomically, and gates each role on its own Selection, local readiness,
and authenticated direct inputs. Recovery remains deadline-bounded and does
not claim distributed atomicity.

### Code and evidence reality

- **Implemented and wired**: generic DI-opaque
  `begin_collaboration -> ACK_CLOSED -> commit_plan`, explicit PREPLANNED
  compatibility, digest-pinned model adapters, replaceable strategy loading,
  `PreSplitFirstStrategy`, signed Provider offers, exact multi-role Selection
  assignments, provider-local GPU admission ledger, asynchronous preparation,
  DAG objects/results, bounded compensation, model-shard retention, and
  exact-prefix derived-state identity.
- **Executed**: the 4 GiB bounded Docker lifecycle at
  `results/spec163-local-docker-20260729_015529` and the real MiniNDN carrier at
  `results/spec163-minindn-matrix-v2-20260729_022847`.
- **Measured**: 59/59 MiniNDN matrix rows, 23/23 gates, 63 row-specific
  evidence references, four runtime assertions, 36 exhaustive bounded
  histories, seeds 163000–163031, five Qwen3 warmups, 25 measured complete
  generations, prompt-specific semantic contracts, raw per-token timing, real
  repository store/fetch, and byte-payload preparation attribution.
- **Deferred**: GPU load/utilization and exact warm-GPU zero-reload evidence,
  because the local host exposes no CUDA device.
- **Not claimed**: distributed Qwen execution, TigerCluster/large-model
  performance, malicious-computation correctness, distributed atomicity,
  deadlock freedom, starvation freedom, or universal optimizer quality.

### Findings

| ID | Severity | Dimension | Finding | Resolution |
|---|---|---|---|---|
| POST-001 | RESOLVED HIGH | State security | The first implementation of `ExactPrefixKvKeyV1` omitted model-semantics, runner, split, prefix length, position, and layout bindings. | The canonical key now binds all required identity dimensions; per-field mutation plus expiry/provider/boot/cache/pin/tenant/requester/layer tests pass. |
| POST-002 | RESOLVED HIGH | Evidence integrity | The first Qwen campaign accepted deterministic nonempty output even when the arithmetic answer was wrong and truncated at 64 tokens. | The accepted campaign requires end-token completion, retains the rejected run, and reports reference consistency separately from human semantic quality. |
| POST-003 | RESOLVED MEDIUM | Harness correctness | The first Docker harness could report PASS despite Provider Tracebacks. | Child exit, crash/Traceback, marker, and temporal-order checks are mandatory; only the later accepted run counts. |
| POST-004 | LOW / DEFERRED | Performance evidence | No local CUDA device exists, so GPU reload/reuse cannot be measured honestly. | GPU fields are null/`DEFERRED_NO_LOCAL_CUDA`; CPU and fake residency are not substituted. A future separately authorized CUDA run may close this optimization row. |
| POST-005 | RESOLVED HIGH | Artifact authority | The first automatic coordinator converted candidate digests to deterministic NDN-looking names and committed without invoking the trusted materializer/publisher boundary. | `PlacementDecision.artifact_preparation` now explicitly selects `GENERATED` or `PRE_SPLIT`; `AutomaticPlanningCoordinator` requires `SplitMaterializer` and `DistributedArtifactPublisher`, validates role/digest/name/deadline bindings, and commits only after successful publication or exact existing-publication resolution. Tests prove `materialize -> publish -> commit` and zero commit on every tested failure. |
| POST-006 | RESOLVED HIGH | Default-policy integration | The strategy supported pre-split catalog input, but the automatic coordinator did not inject a catalog snapshot; catalog registration also stored the composite model digest in the field consumed as the content digest. Exact pre-split reuse therefore could not work end to end. | The coordinator now injects an immutable snapshot from `catalog_snapshot_provider`; registration stores the exact model content digest; an automatic-request test proves an exact catalog hit returns `PRE_SPLIT`, executes `resolve_existing -> commit`, and never calls the failing materializer. |

### Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | One application call; ACK-driven replaceable planning; no mandatory TigerCluster |
| Architecture and ownership | Yes | Core owns generic carrier/transaction; NDNSF-DI owns model/GPU/DAG/state semantics |
| Security/correctness | Yes for declared model | Negative token, permission, NAC-ABE, object, assignment, replay, state, deadline, and recovery gates pass |
| Task executability/cohesion | Yes | T001–T013 close cohesive behavioral gates in dependency order |
| Validation/evidence | Yes with bounded claims | Docker, MiniNDN, bounded histories, raw Qwen generation evidence retained |
| Migration/rollback | Yes | PREPLANNED is explicit compatibility; V1/V2 quarantine and attempt fencing retained |
| Code reality | Yes | Focused tests, full build, pybind load, real Collaboration path, and CodeGraph ownership checks executed |
| GPU optimization claim | Deferred | No CUDA; not an exit blocker under the approved Spec 163 environment rule |

### Evidence limitations

The Qwen3 generations are local CPU reference executions, while MiniNDN uses
byte-sized DI artifacts to exercise the real network and security carrier.
The two evidence layers must not be combined into a distributed-Qwen
performance claim. The history corpus is bounded and observed zero violations;
it is not a formal proof over arbitrary schedules. Signatures authenticate
origin and bytes but do not prove correct computation by an authorized
malicious Provider.
