# Research and Design Decisions

## Evidence Baseline

Spec 168 begins from retained evidence instead of restarting the campaign:

- `specs/162-itiger-qwen36-generation/evidence/t009-full-invocation-20260803.md`
  records TigerCluster job 181948: Qwen3-0.6B, three RTX 5000 nodes, one full
  47-token EOS response, CUDA on every stage, one wire request, no per-token
  collaboration, and no CPU fallback.
- `specs/162-itiger-qwen36-generation/evidence/t009-qwen36-smoke-181951.md`
  records job 181951: Qwen3.6-27B did not reach inference completion because the
  Stage 1 DistributedRepo fetch exhausted adaptive segmented-fetch retries after
  630,800 bytes. Stage 0 and Stage 2 downloaded much larger shards. This is
  negative repository/preparation evidence, not a large-model inference result.
- Spec 167 retains the separate DistributedRepo transport qualification. Its
  goodput rows may establish repository transport behavior under that workload;
  total inference-job duration cannot be substituted for those measurements.

## D1 - Use a distinct Spec 168 identity

**Decision**: Keep Specs 162 and 167 immutable and create Spec 168 for the
deployment-fidelity lifecycle and repair campaign.

**Rationale**: Spec 162 already contains both a successful small-model result and
a failed large-model identity. Spec 167 owns repository throughput. Extending
either would conflate claims and make failed identities mutable.

**Rejected alternative**: Edit the earlier evidence and rerun under the same
identity. This destroys provenance and hides whether code, model, route, or
schedule changed.

## D2 - Use an evidence ladder, not remote trial-and-error

**Decision**: Admission order is:

1. focused unit/contract regressions;
2. real multi-process MiniNDN lifecycle;
3. exact candidate SIF/container preflight;
4. TigerCluster small-model single request;
5. TigerCluster small-model cold/warm repeated requests;
6. TigerCluster large-model single request;
7. optional large-model repeated measurement.

**Rationale**: Each level adds one class of realism. Most state-machine, route,
request-binding, timeout, and repository-range defects should fail before scarce
GPU allocation. TigerCluster remains necessary for physical GPU, node-local
scratch, scheduler, and cross-node behavior.

**Rejected alternative**: Treat a mocked MiniNDN script or a host-NFD smoke as
the local gate. Hidden test-only defaults and shared process state would allow
the exact defects this feature is meant to expose.

## D3 - Reuse immutable artifacts

**Decision**: Reuse the existing SIF, content-addressed model shards, repository
payloads, schedules, and prompts whenever their recorded digests match. Run
directories contain manifests, links, logs, and analysis—not private copies of
model payloads.

**Rationale**: Rebuilding a foundation image or repartitioning unchanged models
does not increase deployment fidelity. It consumes disk, changes the candidate,
and introduces unrelated failure paths.

**Rejected alternative**: Rebuild on every campaign. This prevents valid cold vs
warm comparisons and repeats previously completed work.

## D4 - Plan only after ACK closure

**Decision**: The application submits only an immutable model identity, prompt,
generation policy, and strategy identity. `begin_collaboration()` publishes the
Request and collects side-effect-free ACK capability snapshots. Only after
`ACK_CLOSED` may the strategy inspect the model dependency graph, decide shard
boundaries, place roles, publish missing artifacts, and `commit_plan()`.

**Rationale**: Placement needs current GPU capacity, reachability, load, and
cache residency. Pre-supplied roles/dependencies are a compatibility path, not
the default dynamic API.

**Rejected alternative**: Require `app.yaml` deployment roles or pre-split model
paths in the invocation. That binds the public API to one model family and makes
ACK metadata irrelevant.

**API migration**: `InferenceApplication.request(model=..., input=...,
generation=..., strategy=...)` is the default. `app.yaml` contains identity,
controller/service connectivity, and deadlines only. The former deployment
object path is explicitly named `request_preplanned()`; the positional legacy
form is a counted deprecation shim with a repository-caller migration and
deletion condition, never an automatic fallback.

## D5 - Make model identity content-addressed and adapter-neutral

**Decision**: Compatibility requires model name plus immutable revision/content
digest, tokenizer digest, semantic/adapter identity, dependency-graph digest,
partition digest, precision, and runtime/backend identity. The strategy consumes
an adapter-produced graph rather than LLM-specific layer assumptions.

**Rationale**: A human-readable name such as `Qwen/Qwen3-0.6B` is insufficient
for safe reuse. Graph-aware splitting also permits non-transformer and future
model adapters without moving model semantics into generic NDNSF.

**Rejected alternative**: Match only model name and layer range. This permits
stale revisions, incompatible tokenizers, layouts, precision, and adapters.

## D6 - Remove the global ReadySet barrier from default DI execution

**Decision**: A committed, authenticated plan authorizes the assigned role.
Execution eligibility is local preparation plus the presence of all direct
predecessor inputs. `DATA_DRIVEN_V2` is the default and has no global activation
message. The existing ReadySet path remains only as explicitly negotiated
`LEGACY_READY_SET_V1` compatibility; mixed policy and automatic downgrade fail
closed, and the policy is plan-digest bound.

**Rationale**: Current `ReadySetCoordinator.activate()` rejects an incomplete
set, while `SelectionGatedProvider.execute()` requires that activation. This is
a global barrier and contradicts the data-driven pipeline: Stage 0 should execute
as soon as it is locally ready, and Stage 1 should wake when its own shard and
Stage 0 output are available.

**Security invariant**: Removing the barrier does not remove authorization.
Every role remains bound to request ID, attempt, plan digest, assignment digest,
Provider identity/boot epoch, role, artifact digests, deadline, and authenticated
plan. Wrong or replayed data remains rejected.

**Rejected alternative**: Keep the barrier and shorten a settle timeout. A
smaller fixed wait changes latency but not semantics and still blocks available
work behind an unrelated slow Provider.

**Migration/rollback**: Providers advertise supported policy versions in ACKs.
A rollback is a new explicitly admitted V1 compatibility invocation, not a
mutation of a committed V2 request. Legacy use is counted and is outside Spec
168 acceptance, allowing later removal without a hidden fallback.

## D7 - Treat disk, RAM, and GPU residency as different facts

**Decision**: A shard transitions through verified durable disk, host-memory,
device allocation/load, and usable GPU residency. GPU residency is asserted only
after the adapter has loaded and validated the shard on the assigned device. A
per-request copy in a temporary working directory is not a new durable artifact,
and a `LOADING` status cannot stand in for completed GPU load.

**Rationale**: Current preparation code can verify/cache bytes, copy them into a
request work directory, emit `LOADING`, and return before adapter load. Treating
that as GPU readiness would create false warm hits and obscure duplicate I/O.

**Rejected alternative**: Record one boolean `cachedModel=true`. It cannot
distinguish a Repo download from RAM retention or a usable GPU tensor layout.

## D8 - One durable invocation contains the token loop

**Decision**: The full input token sequence is supplied once and the complete
generated answer is returned by one collaboration. ACK collection, planning,
Selection, model preparation, and assignment are not repeated per token.

**Rationale**: Distributed stage invocation cost is amortized over the request,
matching normal LLM API semantics. Token-level traces remain evidence inside the
invocation, not new wire Requests.

**Rejected alternative**: One collaboration per generated token. That measures
control-plane timeout repeatedly and makes a large model unusable.

## D9 - Use progress-driven deadlines and a first-writer terminal rule

**Decision**: Each bounded operation has a hard deadline and a no-progress
deadline refreshed only by authenticated, monotonic, identity-bound progress.
Terminal outcomes are first-writer-wins and idempotent. A timeout record includes
the last artifact range, byte/segment checkpoint, preparation phase, stage, or
response checkpoint.

**Rationale**: Large transfers may legitimately exceed a fixed 600-second wall
clock while still making progress. Conversely, a stalled 630,800-byte fetch
should fail with a precise checkpoint rather than an opaque request timeout.

**Rejected alternative**: Increase all timeouts. This hides deadlock and slows
diagnosis without distinguishing progress from waiting.

## D10 - Preserve ownership boundaries

**Decision**: Model planning, caching, adapters, GPU preparation, stage
dependencies, and token execution remain in NDNSF-DI. Generic NDNSF changes are
admitted only for a reproducible collaboration/security/request-correlation
defect. DistributedRepo changes require a reproducible manifest/segment/fetch
defect independent of model semantics.

**Rationale**: A framework-level inference state machine would bind NDNSF to LLMs
and make external placement/model adapters harder to implement.

## D11 - Analyze repetitions without statistical overclaiming

**Decision**: Preserve every scheduled row, distinguish cold and warm cache
states, report per-prompt and pooled distributions, and make correctness the
primary acceptance endpoint. Five measured repetitions characterize this fixed
deployment; they do not establish population-wide performance.

The analysis explicitly guards against:

- **Simpson's paradox**: show per-prompt and per-cache-state results before any
  aggregate.
- **Ecological fallacy**: do not infer one request's behavior from job averages.
- **Berkson/collider bias**: retain failed and canceled admitted rows; do not
  analyze only completed invocations.
- **Base-rate neglect**: report counts and denominators for every failure class.
- **Regression to the mean**: do not declare a repair from one unusually fast
  rerun; require the local regression and remote requalification.
- **Survivorship bias**: preserve original failed identities and partial stages.
- **Look-elsewhere effect**: freeze primary endpoints before the campaign.
- **Forking paths**: freeze analyzer, prompt set, exclusions, and success rules.
- **Correlation vs causation**: a warm request being faster does not prove cache
  reuse; require zero transfer bytes and zero load events.
- **Reverse causality**: do not infer that cache residency caused Provider
  selection without retained ACK inputs and strategy decision evidence.

## Open Implementation Findings

These findings are planning inputs, not yet completed repairs:

1. The all-member ReadySet activation path conflicts with FR-009.
2. Artifact preparation currently risks an unnecessary verified-cache to
   per-request work-directory copy.
3. `LOADING` evidence is not yet equivalent to adapter-confirmed GPU residency.
4. The bounded segmented-fetch repair described after job 181951 requires local
   exact-candidate regression and one new remote requalification.
5. The repeated-request schedule has not yet produced a cold/warm distribution;
   job 181948 proves only one complete control invocation.
