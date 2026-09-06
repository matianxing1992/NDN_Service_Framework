# Quickstart: Spec 162 Qwen3.6 Validation

## HISTORICAL HARD STOP — architecture dependency

The following paragraph is the pre-authorization checkpoint retained for
history. It does not describe the explicitly authorized 2026-08-01
requalification jobs documented below. New jobs still require a fresh explicit
authorization and an exactly-once identity.

Spec 162 was originally `PAUSED_DEPENDENCY` on
`specs/163-di-collaboration-planning`. No command below authorizes `sbatch`,
`srun`, `scancel`, requeue, model download, remote artifact mutation, or
promotion. Only local tests/docs and read-only cluster observation are allowed.

## Current operational checkpoint — 2026-08-01

The first authorized Qwen3.6-27B TigerCluster requalification used the real
NDNSF-DI/DistributedRepo path on three RTX 5000 nodes. The immutable model
artifact is the prepared Qwen3.6-27B manifest
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`
(`sha256:81a39eaabb93592d1d48d051f01607bcecd8f94133f29fcd50b27e340397d1fa`
model digest). Its three stage payloads are 18,530,194,174,
15,987,396,002, and 19,274,718,182 bytes (53,792,308,358 bytes total). The
native runtime is
`/project/tma1/ndnsf-di/releases/spec162-t009-6a0fd98f4c19-a001/runtime.sif`,
SHA-256 `5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5`.

The attempted identities and their immutable evidence roots are:

| Job | Identity | Result and lesson |
|---|---|---|
| 181527 | `spec162-submission-t009-qwen36-live-001` | FAILED before inference: host campaign path leaked into the container; bind the campaign as `/shared/<basename>`. |
| 181528 | `spec162-submission-t009-qwen36-live-002` | FAILED before inference: tokenizer resolved to `/shared/tokenizer`; use the bound artifact tokenizer directory. |
| 181530 | `spec162-submission-t009-qwen36-live-003` | FAILED before token generation: the sealed user code ignored `requireEos=false`; provider logs also recorded token-mismatch messages during Repo STORE control traffic. |
| 181531 | `spec162-submission-t009-qwen36-live-004` | FAILED before token generation: wholesale `user.py` replacement mismatched `llm_pipeline_lib.py` (`unexpected keyword argument require_eos`). |
| 181532 | `spec162-submission-t009-qwen36-live-005` | FAILED after 01:22:44: first-token Selection rejected because the sealed provider used a synthetic `--provider-boot-epoch` while Core supplied its native epoch; 0 tokens were generated. |
| 181535 | `spec162-submission-t009-qwen36-live-006` | CANCELLED after a diagnostic fail-fast change was applied globally: normal DistributedRepo STORE control traffic reached `provider token mismatch`; generic Repo collaborations must keep conservative response semantics. |
| 181536 | `spec162-submission-t009-qwen36-live-007` | CANCELLED after all three Repo stage registrations and ACKs: the sealed DI diagnostic path raised `NameError: DISelectionAssignmentV2 is not defined`; the fix uses an explicit runtime-local import. |
| 181537 | `spec162-submission-t009-qwen36-live-008` | RUNNING at the latest checkpoint with fix-008 source `sha256:92cea2ceb92d36c08c74cc94924e53ae65a3b15bdfac5ca8075817d91cf68000`; all three nodes, routes, Repo nodes, selection keys, and the first two 27B stage registrations are ready. |
| 181538 | `spec162-submission-t009-qwen36-live-009` | CANCELLED: the frozen `AckDecision` could not be mutated after construction (`cannot assign to field 'pending_state_ttl_ms'`). |
| 181539 | `spec162-submission-t009-qwen36-live-011` | CANCELLED after registration and DI offers/assignments succeeded; the sealed SIF lacked the updated DI provider module, so fix-010 binds it. |
| 181541 | `spec162-submission-t009-qwen36-live-012` | CANCELLED after diagnostics exposed `DIRoleAssignmentV2` has no `artifact` attribute in the Qwen preparation cache-key path. |
| 181543 | `spec162-submission-t009-qwen36-live-013` | CANCELLED after registration and positive offers: repeated Selection admission attempted a second GPU hold and returned `DI_GPU_CAPACITY_UNAVAILABLE`; no generation claim. |
| 181544 | `spec162-submission-t009-qwen36-live-014` | RUNNING with fix-014: repeated DI ACK admission reuses the existing unexpired offer instead of double-holding GPU capacity. |

All attempts published the three stage files through DistributedRepo before
the user-side failure. The cold path must be reported as separate phases:
NDN chunk transfer, root-manifest/registration commit, provider fetch/cache,
and inference. In the observed runs, 53.79 GB of stage bytes were followed by
roughly 19–22 minutes of job time before the registration record became
visible. This is diagnostic evidence, not a Repo throughput result: the
registration/catalog commit delay is not the same metric as payload goodput.
The formal five-prompt campaign (one warmup plus five measured generations per
prompt) has not run. The live-005 result is a deterministic source/runtime
binding failure, not a Qwen or Repo throughput result.

Live-006 and live-007 are preserved diagnostic cancellations, not discarded
failures. Live-006 demonstrated that terminal-selection fail-fast is a DI-only
opt-in and must not be applied to generic Repo STORE collaborations. Live-007
demonstrated that a sealed image can miss a transitive Python symbol import;
the source bundle now binds the explicit import fix without rebaking the SIF.
The subsequent Repo fix-009 sets a size-aware `pending_state_ttl_ms` on the
artifact ACK so the Core does not expire a consumed ProviderToken during a
multi-GB pull/verify/finalize operation. Its source bundle is staged at
`/project/tma1/ndnsf-di/sources/spec162-t009-live-qwen36-fix-009` with manifest
SHA-256 `ec8212dbe219f492d0e4474837bdb0208fdf2a079ab42c2409a0c0393e65850d`.

Fix-010 through fix-014 are separate immutable source bundles: fix-010 binds
the actual DI provider module into the SIF; fix-011 constructs the frozen ACK
with its TTL; fix-012 records the original preparation exception; and fix-013
uses the Repo registration `objectName` as the model-cache key. Their source
manifest hashes are `78cbc27e6d77f31ceca8c5876fd6054a4b9f922d67a721c91c0cdab6c2898b21`,
`a01801b57d456cdb28862e8750efaa7aa8ae9861815021c9fa4f8ec4b8022868`,
`328c1cbe309b9574fc68019ab4c7208eb2481a6e08048828a4edbebd37f8930e`, and
`344efb1ba8b70b3fdc27182528d3eb39eeb300569a992c0a7c50f0294abbdf95`, and
`54277995fc0392d37aa37b6e49cea785749670252abc916c3f87ed81ac5b07a6`.

The correction is to make `provider.provider_boot_epoch` (the native Core
process-incarnation value) the only Selection Dataflow V2 epoch. The ACK offer,
assignment, GPU ledger, offer issuer, residency record, storage-key epoch, and
DI participant must all use that exact value. A CLI epoch may be retained only
for an explicitly separate deployment-control contract and must be checked
against Core at startup; it must never override Core for Selection.

The detailed lessons and exact evidence paths are maintained in
`docs/NDNSFDI/tigercluster-qwen36-operational-lessons.md` and
`evidence/t009-requalification.md`.

### Native-runtime ABI incident — 2026-08-01

The Qwen3-0.6B smoke was deliberately reduced to the smallest real
three-node workload so runtime failures could be isolated before another
large-model attempt. Jobs 181774, 181779, 181781, 181785, and 181787 are
preserved under `/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/`.
The first scope-key failure was repaired in Core, but replacing only the Core
DSO then exposed a native binding ABI failure (`double free or corruption
(!prev)`) in `SVSPubSub` construction. Rebuilding `_ndnsf` and
`_py_repoclient` together was necessary but not sufficient.

The decisive diagnosis was a mixed NDN runtime: the Core and bindings were
compiled against the host `/usr/local` NDN-SVS/NDN-CXX/NAC-ABE/NDNSD/OpenABE/
Relic libraries, while the Tiger SIF loaded different same-named DSOs. The
headers had the same digest, so import-only probes passed, but the binary ABI
diverged and corrupted allocation state during Provider construction. Binding
the exact seven runtime DSOs used by the build made the minimal constructor
test fail only with the expected “could not connect to NDN forwarder” error;
the invalid free disappeared. This is a runtime-image defect, not a Repo
throughput result.

The durable repair is `jobs/materialize-core-bindings-fix020.sbatch`, which
seals Core, both Python extensions, and the matching runtime DSO hashes into
one SIF. Materialization Job 181788 was submitted as a new identity; its
result and release digest must be recorded before the next smoke. Never mount
an individual replacement Core library into a production SIF and never treat
`import` or `ldd` alone as an ABI gate: run the constructor smoke and then the
real MiniNDN/Tiger harness with the sealed image.

### Context Mode authority-index recovery — 2026-08-01

The Context Mode service itself remained healthy, but its project binding
failed with `expected one project ContentDB ... found 0`. Read-only diagnosis
found the ContentDB and SessionDB intact, while the current project's
`.specify/feature.json`, the Context Mode guide, and the active Spec 167
documents were absent from the file-backed source table. This is an indexing
state failure, not evidence loss in the TigerCluster campaign.

The repair is now tracked as
`scripts/context_mode_index_authority.sh`. It reads the current
`.specify/feature.json`, indexes exactly the pointer, guide, `spec.md`,
`plan.md`, and `tasks.md`, and runs the fail-closed guard. The repaired state
was verified with ContentDB
`/home/tianxing/.codex/context-mode/content/b5bf00f8ae9f1e82.db`: all five
file-backed sources were fresh, hooks were trusted, and the current SessionDB
contained 10 post-restart user prompts. Run the helper after every active-Spec
switch or plan change; Context Mode does not automatically re-index a mutable
Spec pointer.

### Qwen3-0.6B control-plane readiness addendum — 2026-08-01

The coherent fix-025 runtime removed the native ABI abort, but the next
three-node smoke still failed before model execution. Job 181797 received the
User request at Providers 0 and 2 only; Job 181798 (fix-026, with an explicit
User producer route) received it at Provider 0 only. The User failed during
the ACK window, with no Selection, Repo model fetch, GPU load, or generation
receipt on the missing Providers. The immutable partial roots are under
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/`; both failures
remain retained.

The routing change alone was therefore insufficient. A source comparison found
that the previously successful Spec 160 three-node harness waited 120 seconds
after all Providers were ready, while the historical Spec 162 fix-025/026
attempts used shorter waits. Those historical intervals are retained as
control-plane diagnostics only; they are not model or Repo load time and are
not part of the active lifecycle. A new Tiger identity is not a PASS until the
request-first control-plane evidence and a complete generation are present.

## 1. Local structure and compatibility gates

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/162-itiger-qwen36-generation --strict

python3 tests/python/test_spec161_qwen_generation.py
```

After the adapter exists, its focused tests must additionally prove:

- `qwen2` behavior remains unchanged;
- `qwen3_5` hybrid layer selection and state-dict mapping;
- non-thinking text-only prompt formatting;
- strict runtime-version and CUDA fail-closed behavior;
- complete EOS generation and summary filtering.

## 2. Read-only iTiger discovery

```bash
uofm-vpn-status
ssh -o BatchMode=yes itiger \
  "sinfo -p bigTiger -N -o '%N %t %G %m %c'"
```

Expected inventory: RTX 5000 nodes exist and can provide a same-node three-GPU
reference allocation plus three distinct one-GPU candidate nodes.

## 3. Local Docker and single-node SIF gates

Use the tiny controller/NFD/provider/user operation-status workload. Do not
load Qwen in this gate. Then repeat the same workload inside the new coherent
SIF on one RTX 5000 compute node.

The completed T003 candidate is bound to:

```text
OCI=ghcr.io/matianxing1992/ndnsf-di@sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29
SIF=/project/tma1/ndnsf-di/releases/spec162-6216ef2b9525/runtime.sif
SIF_SHA256=6bb55d85bd1244e30d0233d84c753be887e527695422ba47df9ec89c20c1d072
```

The local gate entrypoint is
`jobs/run-local-docker-operation-status-smoke.sh`. The candidate-bound Slurm
jobs are `jobs/materialize-qwen36-runtime.sbatch` and
`jobs/operation-status-sif-smoke.sbatch`. Do not resubmit their frozen `-001`
identities; their first terminal results are Jobs 174578 and 174594.

## 4. RTX-only capacity and artifact gates

Before downloading model bytes:

1. prove allocation-scoped scratch and durable reserve;
2. verify the frozen model and runtime digests;
3. prove one-node/three-GPU full reference placement;
4. construct the three stage packages;
5. strictly load and execute each stage on one RTX 5000 GPU;
6. retain peak allocated/reserved CUDA bytes.

Any OOM or fallback blocks the candidate. It does not authorize quantization,
different layer ranges, shorter output, or an H100 substitution.

T004's immutable no-download probe is Job 174610. Its three-RTX scratch result
passed with about 15.25 TB free, but
`evidence/t004-capacity-decision.json` is `allowed=false` because the cluster
exposes no authoritative `/project` user quota. Do not submit
`jobs/prepare-reference.sbatch` until the decision is regenerated with a
verifiable quota authority and returns no block reasons.

The original T004 decision remains retained. For T005, the user explicitly
authorized the measured global NFS free space as the development experiment's
durable-capacity authority. `evidence/t005-capacity-decision.json` therefore
returns `allowed=true` while still recording `verifiedQuotaBytes=0`; it is not
a user-quota claim.

## 5. Resume sequence — currently blocked

Before any new live authorization:

1. Spec 163 has no audit `BLOCK` and passes its implementation, security, local
   Docker, and MiniNDN gates;
2. T009 requalification creates fresh source/OCI/SIF and evidence identities;
3. the three Qwen stages are registered as an exact pre-split manifest;
4. validated provider ACK state and `PreSplitFirstStrategy` select that
   manifest;
5. local and bounded single-node fake gates pass under the new lifecycle;
6. a new exactly-once live identity receives explicit authorization.

Only then may the sequence return to one preparation, one linked three-node
complete-generation smoke, and a separately authorized campaign. Preserve the
first result of every identity.

Here "preparation" names the historical offline model-artifact Slurm
allocation, not a second NDNSF service call. The accepted inference attempt
must use the Spec 163 deferred path:
`Request -> ACK_CLOSED -> strategy/graph planning -> deferred split materialize
and DistributedRepo publication -> commit_plan/final Selection -> each
Provider's own model preparation -> dependency-driven stage execution ->
Response`. There is no fixed Provider/User settle command and no DI-specific
`PreparationCommit`: ACK closure is the planning input, Selection is the
commit point, and stage readiness is determined by local model readiness plus
available predecessor data.

Do not mark the automatic-planning catalog `ACTIVE` merely because the stage
files exist on `/project`. The default Tiger smoke intentionally starts with
`publicationState=REQUIRES_DISTRIBUTED_REPO_REGISTRATION`: the sealed graph and
stage inputs are available for planning, but no model bytes or Repo root
manifests are published before the first Request. After `ACK_CLOSED`, the
trusted Qwen materializer validates the selected candidate and the User calls
`jobs/register-qwen36-repo.py`; only after all exact stage digest/size/root-
manifest receipts are returned does `commit_plan` publish final Selection.
Use `DistributedRepo.put_file()`/`get_file()` for bounded content-addressed
chunks; the legacy whole-object `put()` path is forbidden for approximately
18 GiB stages. Subsequent token requests reuse the content-addressed
registration and Provider GPU residency without republishing.

The three-node harness has only process barriers (`controller-started`,
`provider-started`, and route readiness). It does not sleep for a fixed
Provider settle or User startup interval. Required evidence is the ordered
`Request_SENT < ACK_CLOSED < GRAPH_READY < DEFERRED_REPO_PUBLISH <
SELECTION_COMMITTED` trace in `user.log`, followed by Provider
`SELECTION_PREPARE` and
`STAGE_EXECUTION_READY`/`STAGE_DEPENDENCY_READY` markers.

The first authorized T005 preparation was Job 175053. It failed before model
download because the preparation container did not expose the installed SDK
path. The corrected script adds
`/opt/ndnsf-app/python` and an explicit writable `HOME`; its non-root exact-OCI
import gate passes. The corrected pre-architecture replacement is now
`RETIRED_UNSUBMITTED`; it cannot be submitted under the old architecture.

The first request-driven correction was Job 181874. It reached
`REQUEST_SENT -> ACK_CLOSED(3) -> DEFERRED_REPO_PUBLISH_DONE ->
SELECTION_COMMITTED`, and Stage 0 fetched and verified 594,357,850 bytes in
7,374.90 ms. It then failed in the application adapter because
`ProviderRuntimeContext.request_id` was missing; the runtime stores that value
as `ndnsf.session_id`. The local source fix adds a read-only `request_id` alias.
The cancelled negative evidence is retained under
`.spec162-submission-t009-qwen3small-deferred-20260802T210549Z-001.partial`;
no complete-generation PASS is claimed and the identity is not being reused.

## 6. T009 requalification checkpoint — 2026-07-29

The current instrumented local candidate is recorded in
`evidence/t009-requalification.md`. It establishes reproducible parent,
instrumentation-seal, local image, source-bundle, and Docker-archive
identities, but it does not create an OCI manifest digest or SIF identity. The
older Tiger-staged `eaf7...` candidate is superseded and must not be submitted;
the current `2b4e...` archive and source bundle are staged read-only and pass
remote full-file/archive and source-seal verification.

Current boundary:

1. no GHCR publication has occurred;
2. no SIF materialization job has been submitted;
3. no Qwen3.6 model weights have been prepared;
4. no three-node smoke or formal campaign has been submitted.

The analysis contract now reports two different units explicitly:

1. a complete application generation, including the first cold answer and the
   25 preregistered measured warm answers;
2. each token-level NDNSF collaboration inside a generation, including every
   planning phase, response wait, end-to-end token-step time, and the Provider
   invariant `release(request N) < ACK=true(request N+1)`.

The next transition requires explicit authorization to publish the exact final
image candidate. After its immutable OCI manifest digest is recorded, prepare
the exact SIF materialization command and request separate Slurm authorization.

The new job renderer is
`jobs/render-t009-sif-jobs.py`. It must consume the raw manifest addressed by
the published digest; a tag or a manifest whose config digest is not the
frozen local image ID fails before any job is rendered. The renderer itself
does not submit a job. Use the current repository copy after publication. The
copy inside the frozen source bundle is archival, predates the final staging
constants, and must not be executed; the rendered plan is hashed and staged as
a separate artifact.

Use the separately sealed formal analyzer described by
`evidence/t009-formal-analysis-contract.json`. Its v3 schema requires complete
requester and three-rank Provider timing coverage and verifies the direct event
order `ACK_TRUE < STAGE_TIMING < RELEASE < NEXT_ACK_TRUE`; do not use the
archival analyzer copy inside the frozen measurement bundle for the final
report.

## 7. Spec 164 Native Overlay Requalification

The pre-Spec-164 runtime SIF is not eligible for current distributed generation
even when the current Python source bundle is bind-mounted. Job 179489 proved
that boundary by failing its policy-build import with a missing
`AdaptiveArtifactTransfer` native symbol. This is distinct from the subsequent
local overlay-build failure: the latter exposed an undeclared `libndn-svs`
compile dependency in the Repo Python extension. The dependency is now
declared in `NDNSF-DistributedRepo/pythonWrapper/setup.py`.

Before materializing a replacement SIF, require the source-addressed native
overlay build and both of its import probes described in the Spec 164
quickstart. A candidate that only passes the older generic runtime probe must
not be promoted or submitted for Qwen generation.

## 8. Fix-027 three-node requalification — 2026-08-01

Fix-027 (Job 181799) used a new immutable source bundle and the sealed
fix-025 SIF after restoring the 120-second Provider settle and 5-second User
startup settle. The source bundle SHA-256 is
`8063bf6b9b57bb8c4fb61684b44ecf3e254d4a57a29852ea240962c672670286`; the
SIF SHA-256 is
`f3da4d2147ab66e125a0695ea4fa88920f7dec7da20e7f9a0e88ce2d0a8cc7ce`; and the
Qwen3-0.6B stage-manifest SHA-256 is
`ef9df35d362da17f61eaf5edd9dead5be8b83523ddfa408d8fd6a42fc5b694fe`.

This run crossed more control-plane and Repo boundaries than fix-025/026:
all three Providers started, `repo-registration-ready` was written, all
three `selection-residency-*.json` records were written, and the User request
was published after the settle barrier. The request was received and ACKed by
Provider 0 and Provider 1, but no ACK/request was recorded from Provider 2.
The generated sample therefore failed before Selection and before any model
fetch, GPU load, token generation, or warm-cache measurement:

```text
no capacity-safe split/provider placement: ...:no feasible Provider for /LLM/Pipeline/Stage/2
```

The planner required 2,910 MiB for Stage 2. The failure is classified as a
missing Stage-2 Provider participation/readiness path, not as
DistributedRepo throughput evidence. Provider-2 logs show the process and
role were ready but no request/ACK event; preserve its route, SVS, and
certificate-validation logs before changing GPU thresholds or placement
policy. The formal result is the `result.json` inside
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix027-173631.partial`.

The next gate is a targeted three-node control-plane probe that proves the
Stage-2 producer/request path independently of model loading, followed by a
new immutable generation identity. Do not relabel Job 181799 or reuse its
request identity.

## 9. Fix-028 User-settle experiment — 2026-08-01

Fix-028 (Job 181800) reused the exact fix-027 source, SIF, and stage manifest,
changing only `SPEC162_USER_STARTUP_SETTLE_MS` from `5000` to `60000`.
All three Providers eventually became ready and the User request was
published after the longer settle. The run nevertheless received only the
Provider-0 ACK and failed before Selection with:

```text
no capacity-safe split/provider placement: ...:no feasible Provider for /LLM/Pipeline/Stage/1
```

This negative control rejects “5 seconds was the sole cause.” The remote
Provider participation remains nondeterministic (fix-027 had Providers 0/1;
fix-028 had only Provider 0). Preserve Job 181800 and inspect certificate
Interest routing, SVS mapping delivery, and per-node route/FIB state before
another generation attempt. Do not change GPU capacity thresholds based on
this result.

## 10. Fix-029/030 ACK and Selection lease boundary — 2026-08-01

Fix-029 (Job 181801, submission
`spec162-submission-t009-qwen3small-smoke-fix029-181103`) used source bundle
`365ac8bf4c6e4473c57d7c01518d8d7f62076eec4a4c5d93f2f67707ce9d4c64` with the
same SIF and Qwen3-0.6B stage manifest. It changed the ACK collection window
to 120 seconds while retaining the 60-second User settle. This removed the
previous control-plane gap: all three Providers logged the same User request
and returned `DI_SELECTION_DATAFLOW_V2_READY` ACKs for Stages 0, 1, and 2.

The run then exposed a separate protocol boundary. The User selection policy
waits for the ACK collection deadline, while each Provider offer lease was
also 120 seconds. Selection validation failed at the boundary with
`offer_expiry(delta_ms=-8)`, before model fetch, GPU preparation, or token
generation. This is an offer-lifetime/ACK-window configuration error, not a
Repo or CUDA result. The captured live evidence remains in the fix-029
partial directory; the job was cancelled only after the failure was proven to
release the three-node allocation.

Fix-030 (Job 181802, submission
`spec162-submission-t009-qwen3small-smoke-fix030-182404`) uses source bundle
`a52af657f5b56c4c7f5ae5f0d8e76e63d2c20f852659bd80db74f8a1ae631f9d` and
decouples the defaults: ACK timeout 120 seconds and Selection offer lease
600 seconds. Do not call the Qwen gate complete until Fix-030 records
Selection, DistributedRepo fetch, CUDA stage preparation, a non-empty
multi-token answer, and analyzer PASS.

Fix-030 reached Selection and proved the model-preparation path on all three
RTX 5000 nodes. The three Providers fetched their assigned content-addressed
artifacts from DistributedRepo (`594357850`, `283192711`, and `625825930`
bytes; fetch times `32644.92`, `17041.52`, and `37386.68` ms), loaded each
stage on `cuda:0`, and reported `cpuFallback=false`. Stage 0 also completed
CUDA execution (`compute_ms=356.63`) and published an 81,816-byte activation.
The request then stopped at the inter-stage collaboration boundary: the
committed plan contained symbolic `pipeline-stage-0-to-1` and
`pipeline-stage-1-to-2` scopes but no encrypted scope-key Data names. Provider
0 logged `Missing collaboration scope key`; Providers 1 and 2 timed out
waiting for the dependency references. This is a confirmed DI plan-sealing
defect, not a DistributedRepo or GPU defect. Captured logs are retained under
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix030-182404.partial/live-logs-captured`.

The source fix publishes one request-scoped encrypted key per dependency edge
after placement and before `commit_plan`, then commits the resulting absolute
Data names together with `key_scopes`. Fix-031 reruns the same workload with a
new source/submission identity and still requires a non-empty multi-token
answer and analyzer PASS.

Fix-031 (Job 181804) used the new scope-key sealing bundle, but this attempt
was a preserved pre-Selection negative control: only Provider 0 received the
first token request and ACKed, while Providers 1 and 2 logged no request/ACK.
The User timed out with `no feasible Provider for /LLM/Pipeline/Stage/1` after
120 seconds. Consequently this run provides no scope-key, Repo, CUDA, or
generation evidence. The exact result is retained at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix031-184255.partial`.

The next run uses a fresh submission identity and a longer Provider
convergence barrier while reusing the same content-addressed source/model
inputs; it must first recover three ACK coverage before evaluating the scope-key
fix.

## 11. Fix-032 terminal result and launcher propagation defect — 2026-08-01

Fix-032 (Job 181805, submission
`spec162-submission-t009-qwen3small-smoke-fix032-185658`) reused the Fix-031
source/SIF/stage identities and requested `SPEC162_PROVIDER_SETTLE_SECONDS=300`.
The wrapper, however, validated neither nor passed that variable into
Apptainer; the inner script therefore used its 120-second default. The run
again reached all Provider-ready and Repo-registration barriers, but only
Provider 0 processed the first token request and returned an ACK. Providers 1
and 2 logged no request/ACK, and the User failed before Selection with
`no feasible Provider for /LLM/Pipeline/Stage/1` after 120008 ms. No scope-key
publication, Repo fetch, CUDA preparation, or generation evidence is
attributable to this run. The immutable result is
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix032-185658.partial/result.json`.

The launcher defect is now fixed locally: the outer rank wrapper validates and
propagates `SPEC162_PROVIDER_SETTLE_SECONDS` to the container, with the same
30–900 second bounds as the inner script. Fix-033 must use a new source and
submission identity and verify the effective settle value from the captured
source/runtime evidence before interpreting ACK coverage.

## 12. Fix-033 terminal result: effective 300 seconds did not restore remote ACK coverage — 2026-08-01

Fix-033 (Job 181806, submission
`spec162-submission-t009-qwen3small-smoke-fix033-190915`, run
`spec162-run-t009-qwen3small-smoke-fix033-20260801T190915Z`) used the corrected
outer wrapper and source SHA
`3ff785c7fb923073578bbc52ad0f244a6618a148fae58a3734d62c5084078f3f`.
The captured Provider process tree confirms that the requested value was
effective: the inner launcher executed `sleep 300` before starting the User.

All Provider-ready and Repo-registration barriers completed, and the User
started after the 300-second barrier. Nevertheless, only Provider 0 processed
the first token request and returned an ACK. Providers 1 and 2 logged no
request/ACK. The User failed before Selection with
`no feasible Provider for /LLM/Pipeline/Stage/1` after 120008 ms. The terminal
result is `FAIL` (exit 1) at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix033-190915.partial/result.json`.

The post-run NFD evidence shows that the aggregate prefix had the expected
multicast strategy and remote nexthops, but the remote faces had zero packets
in and out. This is therefore a control-plane/native request-forwarding
failure, not a Provider-readiness, DistributedRepo-throughput, CUDA, or model
generation result. No Selection, scope-key, Repo fetch, CUDA, or generation
evidence is attributable to Fix-033. The next experiment must instrument or
repair the native `beginCollaboration` request path and verify remote face
counters before retrying the full generation gate.

## 13. MiniNDN-first control-plane recheck — 2026-08-01

Before changing NDNSF internals or resubmitting TigerCluster, the existing
MiniNDN pipeline script was run with the same three-stage fake workload. The
first QuickChecks invocation exposed a harness-only safety omission: the
regression wrapper did not pass the script's explicit
`--test-only-allow-ephemeral-app-state`, so the User stopped at
`RuntimeJournalUnsafeRootError` before publishing a request. This was not a
network or NDNSF result. The regression case now passes that test-only switch;
it does not weaken durable-app production behavior.

With the explicit test-only state acknowledgement, both local paths passed:

* `sudo ... NDNSF_DI_LlmPipeline_Minindn.py --test-only-allow-ephemeral-app-state`
  (MiniNDN with NLSR) completed three role assignments, two scope-key Data
  publications, Selection, and a three-stage response. The recheck recorded
  `LLM_PIPELINE_MININDN_OK`, warmup 305.07 ms, and measured 133.48 ms.
* The same script with `--static-routing-only` also completed the identical
  three-stage path, recording warmup 346.56 ms and measured 154.43 ms.

The second result is important: the current NDNSF collaboration path works in
MiniNDN even without NLSR. Therefore the Fix-033 Tiger failure remains a
Tiger-specific control-plane/topology difference until a route/face comparison
proves otherwise. The next Tiger action must compare the exact SVS group-prefix
routes and live face counters against these two local modes; it must not change
`ServiceUser::BeginCollaboration` or `PublishRequestV2` based only on the
pre-Selection failure.

## 14. Fix-034b terminal result: remote User mapping was lost during certificate convergence — 2026-08-01

Fix-034b (Job 181807, submission
`spec162-submission-t009-qwen3small-smoke-fix034b-194332`, run
`spec162-run-t009-qwen3small-smoke-fix034b-20260801T194332Z`) used source SHA
`5c246622489349d1dbd27c622f503c389471b6b976c57ae78938ff0619737670`.
The Tiger launcher installed the exact SVS group route
`/NDNSF-DistributeInference/example/group` and its multicast strategy on every
rank, and the 300-second Provider barrier plus 60-second User startup settle
were effective. All NFD, Controller, Repo, selection-key, and Provider-ready
barriers completed and the User started.

The run still failed before Selection: Provider 0 received the User request
and returned one valid ACK, while Providers 1 and 2 received no request/ACK;
the User stopped after 120008 ms with `no feasible Provider for Stage 1`.
Provider 1/2 logs show repeated SVS group Data validation. At the first User
mapping publication they reported missing User/Provider certificates, and the
User mapping was never subsequently fetched by the remote Providers. The
captured result is `FAIL` at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix034b-194332.partial/result.json`.

This is a Tiger manual TCP/SVS identity-convergence failure, not evidence of a
generic `BeginCollaboration`/`PublishRequestV2` defect, DistributedRepo
throughput, CUDA preparation, or model generation. MiniNDN (NLSR and static
routing) remains the passing control. Fix-035b therefore changes only the User
startup settle to 240 seconds; it reuses the same source, SIF, and model
artifacts and must again be judged by per-Provider ACK coverage before any
internal NDNSF change.

## 15. Request-driven planning correction — 2026-08-02

The fixed Provider/User settle used by the historical Fix-027--035b runs is
not part of the NDNSF-DI lifecycle. It was useful as a diagnostic barrier, but
it could not distinguish process readiness from request-driven planning and
made the experiment wait even when the request had not been sent. The active
source now enforces this order:

```text
Request_SENT -> ACK_CLOSED -> GRAPH_READY -> deferred split/materialization
-> DistributedRepo publication -> Selection_COMMITTED
-> per-Provider local preparation + input readiness
-> dependency-driven Stage execution -> Response
```

Only local task/input/options validation may precede `Request_SENT`. Model
graph inspection, dependency enumeration, candidate splitting, artifact
materialization, and Repo publication are forbidden before the immutable
`ACK_CLOSED` snapshot. A Provider-started barrier remains only a process and
handler barrier; it is not a timed settle and it does not authorize model
preparation. Stage 0 starts when its own model is ready and the request input
is available; later stages start when their own model and the required
predecessor Data are ready. No command waits for all stages to become ready.

The first submission made with the old source (`181875`, source
`55787da8a8de547ed81ae5d7a0884835adb545f4a68a37ff7cce77aa0500be527e4`) was
cancelled and retained as a negative diagnostic partial. It must not be used
as evidence for the corrected ordering. The one authorized correction reuses
the existing SIF and Qwen3-0.6B stage manifest and uses source
`54091cff7e05c5a1b480a5fc1e55c57471055fd87e1b56cce77aa0500be527e4` under
submission `spec162-submission-t009-qwen3small-data-driven-20260802T214500Z-001`.
The final gate requires the matching `GRAPH_READY after=ACK_CLOSED` marker,
three Provider ACK coverage, per-stage fetch/progress/complete evidence, and
a non-empty final multi-token response; no PASS is inferred from process
startup or Repo registration alone.

Any live status that says the User is waiting in a 300-second Provider-settle
window is therefore not evidence from the active request-driven source. It
identifies a historical Fix-032/033-style bundle (or an incorrectly rendered
bundle) and must be rejected before allocation. The active job now fails closed
on those settle symbols and emits `SPEC162_REQUEST_GATE_OPEN` immediately
before the first User Request. This makes the diagnostic separation explicit:
deployment/route readiness is recorded first, then the actual Request/ACK
exchange determines planning, and model fetch/inference is observed only after
Selection.

## 16. 181876 confirms request-first ordering; no active settle job — 2026-08-02

Read-only TigerCluster inspection found no queued or running job. The latest
submission `spec162-submission-t009-qwen3small-data-driven-20260802T214500Z-001`
(Slurm `181876`) is terminal `FAILED` after 24:14. Its sealed source contains
no settle variable and no `sleep 300`; the captured User log records
`REQUEST_SENT` before `ACK_CLOSED`, then `GRAPH_READY`, Repo publication, and
`SELECTION_COMMITTED`. Therefore a status saying that this identity is still
waiting in a 300-second settle window is incorrect and must not be used as
current cluster state.

The failure is later and independent: the old reference identity expected token
`9909` at index 8 while the current stage/runtime produced `57218`. All three
stages executed on CUDA, dependency receipts were present, and Stage 2
published the token-8 final response before the User rejected it against the
stale reference. Preserve this as `REFERENCE_RUNTIME_MISMATCH`, not as a
deployment/registration or data-dependency failure. The next live identity must
use the current-runtime reference, the new fixed-settle source guard, and the
`SPEC162_REQUEST_GATE_OPEN` marker before attempting another generation.

The corrected source identity is staged, but not submitted:
`/project/tma1/ndnsf-di/sources/spec162-t009-request-first-20260802T223340Z`,
source-manifest SHA-256
`ac5333cef9e4623bab430fc499644810ac2b2c4b5c901c373742634796f1c119`.
It reuses the existing native SIF and content-addressed stage artifact; no
foundation rebuild or model preparation was performed. A new Slurm submission
must remain a separate exactly-once identity and is blocked until the
current-runtime reference artifact is bound to the same SIF identity.

## 17. 181929 request-first 0.6B execution and ACK-strategy finding — 2026-08-03

Submission `spec162-submission-t009-qwen3small-markers-timing-20260803T001500Z-001`
(Slurm `181929`) used source SHA-256
`469b27c7ebd00ce0ecc7381ada3f51800a253fb163048cd12d1260f6f5b1bf51`, the
native runtime SIF SHA-256
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`, and the
path-bound stage-manifest SHA-256
`8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
The run used the existing content-addressed artifacts; it did not rebuild the
foundation image or prepare the model again.

The runtime path itself completed successfully: the User exited zero, produced
47 generated tokens with `status=OK`, `stopReason=EOS`, exact reference match,
and `cpuFallbackCount=0`. Each of the three Providers recorded 47 stage timing
rows, 47 stage-0/1/2 execution rows, and stage 2 published 47 final responses.
The independent post-hoc analyzer recorded `status=PASS`, 141 stage receipts,
94 dependency receipts, and 47 ACK/release pairs per Provider. The official
Slurm result remains `FAIL` because the then-sealed batch script scanned the
sealed source bundle for the literal string `CPUExecutionProvider` and
therefore rejected its own implementation/tests before running analysis. This
is a harness closure defect, not a model, CUDA, Repo, or response failure; the
corrected analyzer and evidence are retained as
`manual-analysis-fixed.json` in the `.partial` directory.

The stage-level cold path was measured, not inferred:

| role | Repo bytes | delivered segments | fetch ms | GPU load ms |
|---|---:|---:|---:|---:|
| Stage 0 | 594,357,850 | 78,205 | 8,174.16 | 5,290.05 |
| Stage 1 | 283,192,711 | 37,263 | 19,995.33 | 6,201.75 |
| Stage 2 | 625,825,930 | 82,346 | 26,382.06 | 6,959.95 |

After the first token, all 46 subsequent token requests had GPU cache hits on
all three Providers (`[46, 46, 46]`) and zero repeated Repo fetches. However,
the ACK-closed placement path still ran 47 times: each token repeated ACK
collection, graph/split evaluation, and Selection; Repo publication occurred
once and the Qwen publisher reused its registration 46 times. The captured
decision marker was `preparation=GENERATED` even on cache-hit requests, which
is semantically misleading and makes the reuse policy invisible to external
strategies.

Therefore the post-ACK `PreSplitFirstStrategy` contract is now explicit:

1. validate the model/graph/candidate identity and ACK cache evidence;
2. score exact `PINNED_GPU`/`RELOAD_SAFE_GPU` residency before RAM, disk, Repo,
   and new materialization;
3. when every selected role has a valid matching cached shard, return
   `ArtifactPreparationMode.REUSE_CACHED` and resolve existing content names
   without materializing or publishing again;
4. fall back to `PRE_SPLIT` for an existing Repo/catalog split, and to
   `GENERATED` only when no valid reusable state covers the selected roles.

This is a placement-strategy decision made after `ACK_CLOSED`; it is not a
fixed Provider settle or a new NDNSF Core wire mode. The production API SHALL
make one complete generation one durable invocation: the full input token
sequence is prefetched once, and autoregressive decode steps exchange
data-plane hidden-state/KV messages inside that invocation. The normative wire
result is one complete Response; a UI may render that completed result after
receipt, but output-token rendering is not a second NDNSF data path and never
reopens ACK/Selection.
The current sequential token requests are retained only as diagnostic
coverage for request IDs, dependencies, and per-request release safety; they
are not the target application API.

The formal analyzer now uses the same post-ACK contract as the smoke analyzer:
`status=true` is the logical ACK admission, not a GPU/storage reservation;
`reservationHeld` is not required; duplicate ACK diagnostics are tolerated;
and a later request may be ACKed before an earlier request's release marker is
flushed. The per-request safety ordering remains strict—its ACK must precede
its own stage timing and release—and the final formal report records
`releaseBeforeNextAckCount=null` rather than inventing a global serialization
barrier. A multi-request formal campaign is therefore required to demonstrate
that the first request is cold while later requests reuse the ACK-advertised
GPU-resident artifacts.

### Post-ACK strategy boundary — clarified 2026-08-03

The strategy discussed above is specifically the strategy invoked **after
`ACK_CLOSED`**. The application call does not pre-split or deploy the model.
The coordinator first sends the Request and obtains the immutable ACK snapshot;
only then does NDNSF-DI inspect the model's ONNX dependency graph, enumerate
capacity-safe split candidates, and jointly assign graph roles to Providers
using the ACK's cache tier, GPU capacity, RTT, bandwidth, and queue evidence.

An ACK cache hint is not itself a reusable artifact reference. `REUSE_CACHED`
is valid only when the selected candidate also has an ACTIVE content-addressed
Repo catalog entry supplying the exact Data names. A Provider may retain a
matching shard on disk while the current Repo process has no active manifest;
that state must take the first-request publication path (materialize/register
the selected graph split, then publish the manifest) before final Selection.
This is why the 181930--181932 formal attempts are retained as failures: they
reached `ACK_CLOSED` and `GRAPH_READY`, but the runtime still treated disk-only
ACK evidence as `REUSE_CACHED` and never committed Selection. The next run must
show the post-ACK decision and artifact-preparation markers before it can be
counted as a distributed-inference result.

### 181941 SDK binding and ACK-window diagnosis — 2026-08-03

The fixed source bundle bound the current DI SDK placement contract into the
sealed runtime, resolving the earlier `AttributeError: REUSE_CACHED` before
candidate resolution. TigerCluster then recorded the complete post-ACK path
for token-level requests 0 through 6: `Request -> ACK_CLOSED -> graph/strategy ->
Repo publish or REPO_REUSE -> Selection -> Response`. The first request
published the content-addressed registration (`44898.24 ms` artifact phase);
later requests reused it (`5.32–10.03 ms`) and Provider ACKs advertised matching
`RELOAD_SAFE_GPU` cached shards.

This run also establishes that the current `ack_timeout_ms=120000` is a hard
wait, not a readiness barrier: all three ACKs arrived in about 1–2 seconds,
but `ACK_CLOSED` was emitted only at 120 seconds. Since the Qwen harness sends
one NDNSF request per generated token, this adds roughly two hours to a
64-token smoke and must be fixed in the NDNSF-DI strategy/closure contract.
The upper-bound deadline remains necessary for missing/late Providers; the
correct repair is an NDNSF-DI-owned validated role-coverage early-close path,
not a pre-request settle or pre-split deployment. Job 181941 was cancelled
after six valid sequential Responses and the next token's ACK collection; it
is diagnostic evidence, not a complete 64-token generation pass.

### 181942 complete runtime response with bounded ACK diagnostic — 2026-08-03

Job 181942 reused the exact 181941 source bundle, SIF, stage manifest, and
0.6B artifacts. It only set `SPEC162_ACK_TIMEOUT_MS=10000` to bound the
diagnostic duration. The runtime produced 47 token-level Requests and final
Responses, one cold Repo publication followed by 46 `REPO_REUSE` decisions, and
an EOS answer with `exactReferenceMatch=true`. All three providers reported 47
CUDA stage executions; stage 1 and stage 2 recorded dependency-ready events.

The outer Slurm wrapper retained `generation-raw.jsonl` and `user-exit=0` but
ended with `state=FAIL`, `exitCode=1` and no `analysis.json`. Therefore this is
complete post-ACK runtime evidence, not a formal campaign PASS. The partial
evidence is retained at:

`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-sdkbinding-10s-20260803T014744Z-001.partial`

The result confirms that the strategy under discussion is strictly after
`ACK_CLOSED`: it consumes ACK metadata and the ONNX graph, resolves or reuses
content-addressed artifacts, and commits Selection. It must not be replaced by
pre-request deployment or pre-splitting. The next fix is the DI-owned validated
role-coverage early-close contract; the timeout remains the upper bound.

### 181943 atomic marker repair and recovered analyzer PASS — 2026-08-03

The follow-up reused the same SIF, stage manifest, Repo registration, and
0.6B artifacts, changing only the source-bundle timing-marker writer. It
recorded 47 parseable timing markers for 47 token-level cycles, one cold Repo
publication plus 46 GPU-cache reuses, and the complete 47-token EOS answer.
Running the retained smoke analyzer against the evidence returned `RC=0` and
`status=PASS`: 141 CUDA stage receipts, 94 dependency receipts, and 47
reservation releases per Provider were covered. The recovered files are
`analysis-recovered.json`, `generation-enriched-recovered.jsonl`, and
`recovered-analysis-sha256.log` in the partial evidence directory:

`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-atomicmarker-20260803T020650Z-001.partial`

The Slurm wrapper still returned `state=FAIL`, `exitCode=1` before its own
analyzer invocation, so the wrapper status is not silently upgraded. The
runtime and independently recovered analyzer prove the post-ACK strategy; the
next maintenance fix is wrapper error propagation/preservation, followed by
the DI-owned role-coverage early-close contract.

### 181944 official wrapper PASS — 2026-08-03

The wrapper fixes were validated in the formally submitted campaign
`spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001` (Slurm job
`181944`, `COMPLETED`, exit code `0`). It reused the existing Qwen3-0.6B SIF,
stage manifest, Repo registration, and model artifacts. The source bundle SHA
is `d2c3f0637e6f79fc60d4c496d20e5bc7d69b21e2370afa62362ac2c162aae1fc`; the
SIF SHA is
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`; and the
stage manifest SHA is
`8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.

The authoritative result is
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001/result.json`:
`state=PASS`, `exitCode=0`. It records a complete 47-token EOS response with
`exactReferenceMatch=true`, 141 stage receipts, 94 dependency receipts, zero
CPU fallback, and 47 release markers per Provider. Fetch progress contained
9/15/24 monotonic events for stages 0/1/2; cold Repo fetch took
8707.04/19418.62/27341.51 ms and cold GPU load took
5522.71/6479.60/6977.68 ms. After the first request, each Provider reported
46 GPU-cache hits. The full response is recorded in the evidence report, not
reduced to a token ID.

The timing marker is now written atomically and the ephemeral bootstrap
credential is removed before the wrapper secret scan. Therefore 181944 is the
formal closure of the 0.6B TigerCluster gate; 181942 and 181943 remain useful
historical diagnostics but are not promoted to PASS.

For application semantics, do not copy the smoke harness's token-level
collaboration loop into a production API. GPT/DeepSeek-style usage is one
durable invocation containing the complete input token sequence. NDNSF-DI
collects ACKs once, runs the post-ACK graph/cache strategy once, sends one
Selection, and performs prefill plus autoregressive decode internally. The
normative result is one complete Response because it amortizes the expensive
distributed control-plane and preparation work. If a caller wants a typing
animation, it is a local presentation of the completed result, not a stream of
per-token NDNSF messages.
The implementation surface is `GenerationRequest` plus
`APPClient.generate()`. Its request envelope carries `generation_mode=FULL`,
while the optional `AckRoleCoveragePolicy` only closes the authenticated ACK
window early when its bounded role hint is covered; graph/split/assignment
remain after the immutable `ACK_CLOSED` snapshot.

### 181948 full-generation requalification — 2026-08-03

The request-first TigerCluster requalification reused the qualified runtime
SIF, the existing Qwen3-0.6B content-addressed stage manifest, and the same
three RTX 5000 nodes. The first two identities (181946 and 181947) reached
three-stage CUDA execution and produced the complete 47-token EOS computation,
but the automatic user path treated the adapter-decoded `bytes` returned by
`AutomaticInferenceHandle.result()` as a raw `ServiceResponse`. Their raw
evidence records the exact error `'bytes' object has no attribute 'payload'`;
they are retained as failed identities.

The corrected identity 181948 calls `handle.response()` at that transport
boundary. Its immutable raw evidence contains one `GenerationRequest`, one
wire Request, one ACK_CLOSED/Selection lifecycle, three Repo fetch completions,
three CUDA-resident stages, and one complete decoded response:

```text
submission: spec162-submission-t009-full-20260803T044700Z-003
job:        181948
source:     ed6a7e0168c3d63720dbfe4f58c4fa2e6ccb796550cbd5721268c7dabbc02513
SIF:        1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368
stages:     8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5
runtime:    status=OK, exactReferenceMatch=true, generatedTokenCount=47,
            stopReason=EOS, wireRequestCount=1, tokenRequestCount=0
nodes:      itiger09, itiger10, itiger11
```

The Slurm wrapper remains recorded as `FAIL` because it used the older
analyzer. The corrected analyzer (source bundle SHA
`3cee5d3d87cc9e0c48a322c4f0bf9b6f6f3bf9dc9d1450a6f5e89fe02b858194`) was run
independently over the retained 181948 partial evidence and returned
`status=PASS`; it accepts the sealed
runtime's `LLM_PIPELINE_QWEN_MODEL_RESIDENCY` marker and requires the stage-0
terminal marker once (not once per generated token). The wrapper state is not
rewritten. This is a complete runtime PASS plus analyzer-revalidation PASS,
not a claim that the old wrapper itself passed.

The result confirms the intended API granularity: the complete input token
sequence is sent once; model preparation, internal hidden-state/token exchange,
and autoregressive decode remain inside the selected distributed invocation;
the application receives one complete Response. Any future response streaming
must use ordered chunks of that same invocation and must not reopen ACK,
planning, or Selection per output token.
