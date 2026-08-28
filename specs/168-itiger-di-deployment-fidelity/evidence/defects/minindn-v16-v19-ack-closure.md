# MiniNDN v16-v22 Defect Lineage: ACK Closure Through Provider Preparation

**Status**: four independent lifecycle defects are frozen. The Core ACK race,
same-identity process collision, request-first offer-window mismatch, and late
runtime-capability discovery are repaired locally. A runtime-compatible real
MiniNDN run is still required before exact-SIF admission.

## Frozen inputs

All seven runs reused the existing Qwen3-0.6B CAS stage bundle, tokenizer,
workload, three-stage topology, and model identity. No model was split, copied,
or rebuilt between these diagnostic runs.

```text
model: Qwen/Qwen3-0.6B
revision: e6de91484c29aa9480d55605af694f39b081c455
model identity: sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db
stage bundle: sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5
workload: sha256:72ac067699263434995a9d0615ccae2de3eb90913b1dcd3c96f0a3f0a38b41b4
```

The original run directories remain unchanged under
`results/spec168-real-minindn-dev/`.

## Boundary-by-boundary evidence

| Run | Newly exercised boundary | Last proven checkpoint | Failure |
|---|---|---|---|
| `20260803T1840Z-qwen3-small-v16` | request-first planning followed by dynamic Repo publication | main Request collected 3 ACKs; all three stage artifacts were published; `ARTIFACTS_READY` and final Selection commit were emitted | the publisher created a second same-identity `ServiceUser`; its newer SVS session superseded the original inference session, so Providers did not accept the later main Selection and the Request expired |
| `20260803T1905Z-qwen3-small-v17` | same durable `ServiceUser` reused for inference and Repo control | main Request collected 3 ACKs; all three Repo Providers logged one `REPO_ARTIFACT_ACK_ACCEPT` | artifact publication returned a generic backend error before any model bytes transferred |
| `20260803T1915Z-qwen3-small-v18` | backend exception detail enabled | same as v17 | exact error became `repo-store-insufficient-cover` |
| `20260803T1925Z-qwen3-small-v19` | ACK-coverage diagnostics added | main Request collected 3 ACKs; each Repo Provider accepted the store Request | User froze `candidateCount=0 successfulCount=0 eligibleCount=0` for a 594,357,850-byte stage |
| `20260803T1945Z-qwen3-small-v20` | Core defers ACK closure while Collaboration ACK decrypt/authentication is in flight | the repaired Core regression suites passed, and all Repo processes again accepted the Request | Repo ACK closure was still empty; `stale=2` exposed that Repo and compute processes were publishing under the same Provider base identities but different SVS sessions |
| `20260803T2015Z-qwen3-small-v21` | Repo and compute processes use distinct identities and bootstrap tokens | all three artifacts were published (`594,357,850`, `283,192,711`, and `625,825,930` bytes), `ARTIFACTS_READY` was emitted, and the final three-Provider Selection was committed | all compute Providers rejected the same Selection before fetch with `offer_expiry(delta_ms=-17639..-17642)`; the generation ended `REQUEST_FAILURE`, zero tokens |
| `20260803T2055Z-qwen3-small-v22` | the signed offer remains valid through final Selection | all three artifacts were published, Selection committed, and Providers fetched the exact three objects in `16,677.09`, `34,431.68`, and `67,032.66` ms | every Provider failed before load with `ModuleNotFoundError: transformers.models.qwen3`; the host runtime was Transformers 4.46.3, while the frozen stage requires the Qwen3 module |

v17-v20 are not repeated model-transfer attempts: all terminate at the first
Repo ACK-closure boundary, before artifact payload transfer.

## Root cause 1: asynchronous ACK closure

`ServiceUser` decrypts/authenticates ACKs asynchronously. Before this repair,
the pre-decrypt path incremented `ackDecryptsInFlight` only for legacy/custom
`acksHandler` and `ackCandidatesHandler` calls. A deferred Collaboration has
neither handler; it uses `collaborationAckClosedHandler`. Consequently:

1. a Repo Provider could accept a Request and publish an ACK before the ACK
   deadline;
2. the User could observe that encrypted ACK but fail to mark its decrypt as in
   flight;
3. the ACK timer could freeze the immutable `ACK_CLOSED` snapshot with zero
   candidates;
4. decrypt/authentication could finish only after the empty snapshot was
   already authoritative.

This explains why Provider-side `REPO_ARTIFACT_ACK_ACCEPT` and User-side
`candidateCount=0` coexist. Increasing the 3-second window would reduce the
probability but would not remove the race.

## Root cause 2: one identity used by two independent processes

v20 proved that the decrypt repair was necessary but not sufficient. Each
MiniNDN node launched its Repo service and compute stage as separate processes
under the same base Provider identity. The later compute process established a
newer SVS session. `ServiceUser::isFresh()` keeps the greatest session ID for a
base producer prefix, so Repo ACKs from the older process could be rejected as
stale even though the Repo application had accepted the Request.

The deployment now gives Repo processes `/example/llm-pipeline/repo[/1,/2]`
and compute processes `/example/llm-pipeline/provider[/1,/2]`. Policy,
certificates, bootstrap tokens, routes, MiniNDN launch arguments, and the
TigerCluster inner launcher all preserve this separation. v21's complete
three-artifact publication is the real-network proof that this boundary moved.

## Root cause 3: offer validity shorter than admitted preparation

v21's Provider ACK diagnostics recorded
`pendingStateTtlMs=899945..899946` with `providerLimitMs=900000`, while the
MiniNDN launcher supplied a fixed `selection-offer-lease-ms=120000`. Dynamic
Repo publication happened between ACK closure and final Selection and took
longer than 120 seconds. The request deadline and Provider pending state were
still valid, but the exact signed ACK offer was already about 17.6 seconds
expired when Selection arrived. All three Providers therefore correctly failed
closed before model fetch.

This is not a network timeout and must not be repaired by an arbitrary sleep or
by weakening Selection/ACK binding. In the current single-ACK-closure attempt
there is no offer-renewal round, so the admitted request-first offer lease must
cover the maximum preparation window.

## Root cause 4: runtime capability was advertised before it was proven

v22 proved that the offer-window repair moved the boundary through exact Repo
fetch on all three Providers. The host Python runtime nevertheless used
Transformers 4.46.3, which has no `transformers.models.qwen3` package. Because
`--lazy-qwen-load` deferred every model-family import until after Selection and
fetch, each Provider published a positive ACK and transferred hundreds of MB
before discovering that it could not construct the selected model.

This is a Provider capability-truthfulness defect, not an artifact transport
failure. The retained OCI image `ndnsf-di:spec165-minindn-cpu-gate` contains
Python 3.10.18, Transformers 5.14.1, Torch 2.6.0, and the Qwen3 module; the
host runtime does not. The next real MiniNDN candidate must execute in a
runtime with that same capability rather than installing an untracked global
package or weakening the real-model gate.

## Repair and regression

The Core now uses one `shouldTrackAckDecrypt(PendingCall)` predicate everywhere
ACK decrypt tracking, completion, selection, and timeout deferral are decided.
It includes every Collaboration, so an ACK observed before the deadline keeps
`ACK_CLOSED` open until decrypt/authentication finishes.

Test-first evidence:

- RED: the new `DeferredCollaborationTracksAckDecryptBeforeClosure` regression
  failed to compile because the production predicate did not exist;
- GREEN: `unit-tests` built successfully with the system linker and the focused
  regression passed;
- the complete `GenericDynamicApi/CollaborationStatus` suite passed 6/6;
- automatic collaboration planning passed 15/15;
- collaboration artifact backend passed 6/6;
- Spec 168 real-MiniNDN gate contracts passed 8/8.

The repository-wide default build remains affected by a pre-existing host
toolchain mix: Homebrew `ld` 2.47 is ahead of the system libraries and fails to
resolve system Boost/GLib/OpenSSL symbols. Prefixing `/usr/bin:/bin` selects the
system linker and builds `unit-tests` successfully. This linker issue is
environmental evidence, not a failure of the ACK-closure repair.

The Qwen request-first Provider now fails fast when its offer lease is shorter
than its maximum preparation window. The normal Provider CLI defaults are both
600,000 ms; the real-MiniNDN defaults are both 900,000 ms. Explicit shorter
leases can no longer enter a cold request that is guaranteed to expire before
its admitted preparation window closes.

Test-first evidence for the offer-window repair:

- RED: `test_request_first_offer_lease_covers_preparation_window` failed with
  `AttributeError` before the production validator existed;
- GREEN: Qwen Repo selection preparation passed 2/2, including cold fetch and
  warm GPU reuse;
- Qwen preparation passed 12/12;
- sealed live-source contracts passed 17/17;
- Spec 168 real-MiniNDN contracts passed 8/8;
- deferred planning passed 7/7;
- Provider dependency-deadline tests passed 4/4.

The generic DI offer issuer also rejects a Request at ACK admission when the
remaining plan deadline exceeds the signed offer lease. Its normal default now
matches the one-hour pending-state limit. The TigerCluster launcher defaults
the lease and Provider maximum preparation window to the exact Request timeout
instead of allowing a positive offer to expire mid-publication.

The Qwen Provider now requires an explicit immutable model family on the
request-first path and imports the corresponding configuration/model modules
before constructing the network Provider or emitting `PROVIDER_READY`. The
MiniNDN launcher supplies `qwen3` for Qwen3-0.6B; the Tiger launcher derives
`qwen3` or `qwen3_5` from the frozen stage-manifest profile. On the current
host this preflight deterministically produces
`QWEN_TRANSFORMERS_MODEL_TYPE_UNAVAILABLE:qwen3` before any Request or model
transfer.

Additional test-first evidence:

- RED: the Provider preflight test failed because no runtime probe existed;
- RED: a generic issuer with an 8,001 ms Request remainder and 5,000 ms offer
  lease incorrectly returned a positive ACK;
- GREEN: Selection Dataflow V2 passed 23/23;
- Qwen Repo preparation passed 3/3;
- Qwen stage contracts passed 16/16;
- Qwen/Tiger preparation contracts passed 12/12;
- sealed live-source contracts passed 17/17;
- Spec 168 real-MiniNDN contracts passed 8/8;
- deferred planning passed 7/7.

v22 additionally exposed that Provider `PREPARATION_FAILED` statuses did not
drive the User to a prompt terminal result within more than one minute. The run
was interrupted only after all three fetches and failures were frozen. That
failure-propagation behavior remains the next separate lifecycle defect; it is
not conflated with the runtime-capability repair.

## v23: V2 artifact identity crossed into the V1 execution-spec path

The thin current-source candidate
`ndnsf-di:spec168-v23-native-296faa3ad80c` passed its Qwen3 import/runtime
preflight and ran the unchanged Qwen3-0.6B workload in real MiniNDN. The run is
retained at
`results/spec168-real-minindn-dev/20260803T2020Z-qwen3-small-v23-oci`.

It proved all of the following before its terminal failure:

- one deferred Request collected three ACKs and committed one exact plan;
- all three immutable Repo objects were registered and fetched by their
  assigned Providers (594,357,850, 283,192,711, and 625,825,930 bytes);
- Stage 0 and Stage 1 loaded their exact verified Qwen packages on the explicit
  CPU_LOGIC local gate;
- the same request ID, partition digest, graph digest, model-content digest,
  role, and artifact digest survived through those checkpoints.

Stage 0 and Stage 1 then changed from successful DI-owned fetch/load to
`PREPARATION_FAILED` with `failed to fetch execution spec`. The reported name
was the same DistributedRepo model object they had just fetched. The User
therefore timed out at 900,013.5 ms and retained one failed generation record
with zero output tokens. Stage 2 completed Repo fetch but did not reach a
runtime-ready marker before terminal cleanup; that is not classified as a
successful load.

The root cause is a lifecycle ownership collision. After the committed V2
participant had verified and loaded its immutable shard, the generic Provider
wrapper interpreted nonempty `assignedArtifact` using the quarantined V1
meaning of an embedded `ExecutionArtifactSpec` and called
`prepare_execution()` a second time. In V2 the same field is an opaque exact
artifact identity owned by the NDNSF-DI participant/adapter. The existing unit
tests used either an empty assigned artifact or an already-present local path,
so they bypassed the real deployment branch.

The repair makes every opaque-selection V2 invocation create only an
adapter-facing execution shell after participant preparation. Its mandatory
`runtime_preparer` must then bind the exact role, adapter, backend, device, and
artifact digest and issue runtime evidence. The V1 `prepare_execution()` fetch
remains available only outside the V2 participant path. A new regression uses
a nonempty Repo-style assigned artifact and fails if it enters the V1 fetch.
The local gate also distinguishes an explicitly assigned CPU_LOGIC backend
from an accidental CUDA-to-CPU fallback.

Focused post-repair evidence:

- Spec 168 Provider generation and V2 artifact ownership: 9/9;
- Qwen Repo selection preparation and CPU_LOGIC classification: 4/4;
- Selection Dataflow V2: 23/23.

## Next admission boundary

Build one new thin current-source overlay from the same retained parent and
reuse the exact v23 model, tokenizer, stage manifest, workload, and topology.
Run exactly one v24 real-MiniNDN campaign. It must prove all three roles reach
runtime readiness and data-driven stage execution; any Stage 2 load stall must
be frozen and diagnosed separately rather than hidden by increasing a timeout.
Materialize the exact SIF only after this boundary passes; do not enter
TigerCluster yet.

## v24-v26: runtime preparation, committed dependencies, and the 8 GiB host boundary

v24 cannot be used as product evidence. A separate concurrent local experiment
issued `killall nfd` at the same instant as its network failure. v24b repeated
the unchanged workload after the host became quiescent and exposed a real
adapter defect: `warm_qwen_transformer_stage()` required CUDA even though the
committed role assignment explicitly selected CPU. The warmup now executes on
the assigned device and rejects only an actual assignment/device mismatch.
Focused CPU/CUDA warmup coverage passes 5/5.

v25 then fetched and loaded all three exact Qwen3-0.6B stage objects on CPU,
but Stage 0 attempted to publish on the startup policy scope
`tensor-0-dfb7e82149ba03ac`. Core had committed and distributed the dynamically
planned scope key `tensor-0-a6c574e2293c80c7`. The Provider handler was still
building `RoleDependencyView` from the static startup graph because
`DIRoleAssignmentV2` carried only a dependency digest and input-scope names,
not the committed edge descriptors. A second defect allowed an empty large
Data name to be wrapped and logged as if publication had succeeded.

The V2 assignment now carries canonical role-local dependency edges, including
producer/consumer roles, key scope, topic prefix, tensors, object-name template,
and expected transfer bounds. The assignment fails closed when the edge tuple
is absent, excludes the role, duplicates a scope, disagrees with required input
scopes, or does not match its dependency digest. A V2 Provider constructs the
handler dependency view only from this committed tuple; the static graph remains
the explicit preplanned compatibility path. Empty large-object publication now
raises before a reference or success marker can be emitted.

Post-repair focused evidence:

- Selection Dataflow V2: 24/24;
- Spec 168 Provider generation: 9/9;
- automatic collaboration planning: 15/15;
- Qwen Repo preparation and device warmup: 5/5;
- Python compile and `git diff --check`: pass.

v26 reused the exact v25 image, Qwen3-0.6B stage bundle, tokenizer, workload,
topology, and Repo payload under `--memory=6g --memory-swap=7g`. It proved the
repaired real-network boundary:

- one request ID collected three ACKs and closed one immutable ACK snapshot;
- graph/candidate resolution and three Repo registrations occurred only after
  `ACK_CLOSED`;
- Core published the dynamic keys `tensor-0-a6c574e2293c80c7` and
  `tensor-1-853f45f2c280507e`;
- final Selection carried the larger dependency-bearing assignments;
- Stage 0 fetched 594,357,850 bytes in 24,730.16 ms, prepared on CPU, and
  published epoch 0 on the committed dynamic scope;
- Stage 1 fetched 283,192,711 bytes in 78,253.70 ms, prepared independently,
  consumed Stage 0 epoch 0, and immediately published its own epoch 0;
- Stage 2 fetched 625,825,930 bytes in 99,962.10 ms.

This closes the v25 dynamic/static scope defect and proves data-driven
Stage 0 to Stage 1 execution without an all-role readiness barrier. It does not
prove a complete three-stage response on this host. While Stage 2 was loading,
the 6 GiB container cgroup invoked the OOM killer twice: PID 1292900 (Stage 0,
about 1,031,396 KiB anonymous RSS) at 17:17:03 and PID 1293156 (Stage 2, about
2,086,404 KiB anonymous RSS) at 17:17:06. The remaining process could not
complete the graph, so the run was deliberately terminated with exit code 130
instead of waiting for the 900-second request deadline.

This is a measured capacity boundary, not a protocol timeout or a reason to
raise the local memory cap on an 8 GiB host. Future local real-MiniNDN gates
must use a structurally faithful tiny Qwen fixture whose three simultaneous
role runtimes fit below the 6 GiB cgroup limit. The exact Qwen3-0.6B three-stage
capacity run belongs on TigerCluster, after the tiny fixture and exact-SIF
logic gates pass.

## Revised next admission boundary

Create one content-addressed tiny Qwen stage bundle for local logic validation;
do not copy the existing 1.5 GiB payload into another run directory. It must
retain three roles, two dynamic dependency edges, real Repo publication/fetch,
real transformer forward execution, full multi-token generation, one request
ID, and the same V2 Selection contract. Require a sampled container working
set below 6 GiB and zero cgroup OOM events. Then validate the same source in the
exact SIF. Only after both gates pass may TigerCluster run Qwen3-0.6B on three
GPU nodes.
