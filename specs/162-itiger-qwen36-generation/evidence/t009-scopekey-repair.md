# T009 Qwen3.6 scope-key repair evidence

Date: 2026-08-02

## Scope

This note records the TigerCluster smoke attempts after the Qwen3.6 provider
model-residency repair. No foundation image, SIF, or model shard was rebuilt.
The formal five-prompt campaign was not submitted.

## Attempt 181849: dynamic scope mismatch

Submission: `spec162-submission-t009-qwen36-scopekey-20260802T111954Z`

The run passed source/SIF/shard integrity checks, NFD and Repo barriers, and
registered all three Qwen3.6 shards. Each provider loaded its shard once to
CUDA and later reported `MODEL_RESIDENCY cacheHit=true load_ms=0`; stage 0
also reported `model_cache_hit=1`, `model_load_ms=0`, `device=cuda:0`, and
`cpuFallback=0`.

Execution stopped because the static provider dependency graph still named
`pipeline-stage-0-to-1` and `pipeline-stage-1-to-2`, while automatic planning
created the adapter-derived scopes `tensor-0-dfb7e82149ba03ac` and
`tensor-1-aa6de4efc3538d07`. Providers therefore rejected the named scope key.
The job was cancelled after this diagnosis. Partial evidence is retained on
TigerCluster at:

`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-scopekey-20260802T111954Z.partial`

## Repair

`build-generation-policy.py` now aligns the Qwen policy with the automatic
planner: service `/AI/LLM/Pipeline/Fake`, roles `/LLM/Pipeline/Stage/{0,1,2}`,
topic prefix `/activation`, and the two adapter cut-edge-derived scopes. The
repair is in source bundle:

`spec162-t009-qwen36-scopekey-802f7b563675ddbc2ec2dab714152466c0a7a2a83ca7b4b0626ea5f05ed4747f`

The remotely verified policy contained exactly the two `tensor-*` scopes and
the cut edges `hidden-layer-20-to-21` and `hidden-layer-41-to-42`.

## Attempt 181850: Repo registration bottleneck

Submission: `spec162-submission-t009-qwen36-scopekey-20260802T121636Z-002`

Source, SIF, and shard checks passed, and the repaired policy was verified
remotely. The job never reached provider or user startup and produced no
`repo-registration.json` marker. At cancellation (`00:18:54` elapsed), Slurm
reported approximately 319.7 GB disk read and 50.8 GB disk write for the
53.8 GB total shard payload. This is roughly 5.9x read amplification before
registration completed, so the run was cancelled rather than left consuming
the three RTX5000 nodes.

Partial evidence is retained at:

`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-scopekey-20260802T121636Z-002.partial`

## Attempt 181852: native file-producer SIF and dependency-timeout smoke

Submission: `spec162-submission-t009-qwen36-native-repo-20260802T141500Z-001`

This smoke reused the existing 53,792,308,358-byte Qwen3.6 stage artifacts,
tokenizer, policy/source bundle, and automatic planning manifest. It did not
rebuild the foundation image or prepare model shards. The only runtime image
change was the native `NativeFileSegmentedObjectProducer` overlay, built with
the Ubuntu 20.04/GCC 9/Boost 1.71 compatibility toolchain and materialized as
SIF:

* source SHA-256: `4e9350f07ef4854d4324e727412a46401b13686dfd76d0cc073b550a5d998768`
* Dockerfile SHA-256: `0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92`
* native extension SHA-256: `cd233289fdc885d238876907234f23f55f3b43898dea21e9895c7da3ab69d2fb`
* SIF SHA-256: `1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`

All source, SIF, stage-manifest, repository-registration, and CUDA-residency
barriers passed. The three final shard files were present and loaded to
`cuda:0` with `cpuFallback=false`; the provider preparation fetch times were
996,574 ms, 881,324 ms, and 1,112,367 ms for stages 0, 1, and 2 respectively.
The run was then cancelled after a deterministic execution failure:

* Stage 1 started before Stage 0 had published `tensor-0-dfb7e82149ba03ac` and
  failed after its fixed 30,000 ms dependency wait.
* Stage 2 consequently failed after its fixed 30,000 ms wait for
  `tensor-1-aa6de4efc3538d07`.
* Stage 0 later completed successfully and published 380,824 bytes in
  1,261.59 ms, but this was already after Stage 1's deadline.

The partial result is retained at:

`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-native-repo-20260802T141500Z-001.partial`

The measured failure is a data-dependency scheduling/deadline defect, not a
model-cache, CUDA, or model-integrity failure. It also shows that the
persistent file stream overlay alone does not remove the earlier repository
byte-amplification bottleneck; the new smoke reached registration but still
spent roughly 15--19 minutes fetching each shard.

## Conclusion

The scope-key mismatch has a concrete policy repair, and Repo registration
now passes with the native overlay. End-to-end inference is still blocked by
the fixed dependency wait: downstream roles must use a progress/deadline
derived from the request deadline and dependency readiness, rather than a
30-second constant that expires while another role is still preparing. The
next implementation should repair that scheduler/adapter contract and add a
MiniNDN regression where Stage 1 starts before Stage 0 but succeeds when the
declared dependency is produced before the request deadline. Only after that
passes should the TigerCluster smoke be rerun and the formal
warmup-plus-five-measurement campaign be unlocked.

## Deadline repair and local recheck — 2026-08-02

The Python DI provider now derives dependency reference/fetch/Future wait
timeouts from the signed `DISelectionAssignmentV2.deadline_ms`, with a
one-second response safety margin. Legacy non-V2 contexts retain a bounded
30-second fallback; an expired V2 deadline fails closed. The Qwen transformer
and ONNX handlers use this same remaining-deadline value instead of fixed
30/60-second waits. The source identity includes both the generic DI provider
and Qwen provider modules so this behavior cannot be hidden by an incomplete
experiment manifest.

Focused tests passed:

* `tests/python/test_ndnsf_di_provider_deadline.py` — 3 tests;
* `tests/python/test_spec162_qwen36_repo_prepare.py` — 1 test;
* `tests/python/test_ndnsf_di_automatic_collaboration_plan.py` — 8 tests.

The real MiniNDN Gate B recheck passed with the content-addressed prepared
Qwen3-0.6B artifacts and the current source identity:

`results/spec165-minindn-deadline-recheck-user-2/20260802T151603Z-c92c19b6`

It recorded six measured requests, two warmups, three selected roles, and 64
stage output events per role. No TigerCluster job was submitted by this local
gate. The corresponding source revision is
`f60385a23a6845b094233f34e32ad283664cbbab+spec165:ebb85ac715c77663319c3d42df429d4616ebe4baf19b2a781a2fe37c270780c5`.

## Attempt 181853: deadline-aware Tiger smoke

Submission: `spec162-submission-t009-qwen36-deadline-20260802T1515Z-001`

This job uses source bundle
`spec162-t009-qwen36-deadline-3a31ffe973ecf850a5cbc218124579603d7847ec9888bacc737b66c727930c08`, the existing native-overlay SIF
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`, and the
existing Qwen3.6-27B stage manifest
`cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`. No
foundation image or model preparation was repeated. The job was submitted as
Slurm 181853 on `itiger[07-09]`; its partial evidence root is:

`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-deadline-20260802T1515Z-001.partial`

At the latest checkpoint it had passed source/SIF/stage checks, loaded all
three NFD instances, and completed the cross-node route barrier; execution
and final inference status was not produced; this checkpoint is not a
successful generation result.

## Route-qualified Repo follow-up — 2026-08-02

The next smoke used submission `spec162-submission-t009-qwen36-route-20260802T1052Z-001` (Slurm job `181854`). It reused the same native-overlay SIF and Qwen3.6-27B stage manifest; only the source bundle changed (`bfdfb98e9e749b4dceddd674887da6c2038ac3e1d0daa55edba3f72d97bd2fbd`).

The exact route to the Repo producer root identity (`.../provider/NDNSF-ARTIFACT`) passed the route barrier. Source, SIF, stage integrity, Repo registration, all provider ACKs, all Selection handlers, and all collaboration handlers passed. The failure occurred after control-plane acceptance during concurrent large-object retrieval. Before intentional cancellation at `00:58:54`, Stage 0 reached about 18.00/18.53 GB, Stage 1 about 0.26/15.99 GB, and Stage 2 about 0.042/19.27 GB. Stage 2 emitted `ArtifactApiError INTERNAL_ERROR: artifact backend failed`; no GPU residency or final response was reached.

The retained result is `/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-route-20260802T1052Z-001.partial/result.json` (`state=CANCELLED`, `exitCode=130`). This is negative evidence, not a formal campaign result. The exact route is not sufficient: the Repo still needs fair concurrent segmented-read behavior (or a documented admission policy), and its stable API currently hides the native reason. `NDNSF_ARTIFACT_DEBUG_ERRORS=1` is now an opt-in trusted diagnostic switch that preserves the normal bounded public error while exposing the local/native exception for the next small-model investigation.
## Small-model request-ID requalification — 2026-08-02

Submission `spec162-submission-t009-qwen3small-requestid-20260802T171609Z-001`
(Slurm `181859`) reused the existing native-overlay SIF and the existing
Qwen3-0.6B stage manifest. No image or model preparation was repeated.

* source bundle:
  `10617d5d5cef7a84efb8da7f2c8a607bb26a7af6fc15e38a2ccd56f8187c2266`;
* SIF:
  `1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`;
* stage manifest:
  `eb4e9185a439a65684959a3ee3078e4254f4fb45704b5f305cc05c7434086dd8`;
* retained result:
  `/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-requestid-20260802T171609Z-001.partial/result.json`.

Source/SIF/stage integrity, NFD, routes, Repo registration, provider startup,
ACK collection, Selection, and distributed response delivery all progressed.
The live User log recorded two valid responses with the wire request name
carrying the NDN leading slash while the application request ID remained
slash-free. The repaired comparison accepted both forms:

* token 0: `ack_collect_ms=120000.44`, `response_wait_decode_ms=50767.64`,
  `token_step_total_ms=170793.84`;
* token 1: `ack_collect_ms=120000.48`, `response_wait_decode_ms=104.45`,
  `token_step_total_ms=120129.62`.

Neither response produced `distributed token response request ID mismatch`.
The job was intentionally cancelled while token 2 was in progress because the
smoke harness waits the full 120-second ACK window for every token; running all
64 tokens would turn this diagnostic into a multi-hour run. The retained result
therefore correctly reports `state=CANCELLED`, `exitCode=130`, and is not a
formal generation pass. The node application logs were emitted in the live
container and were not copied before Slurm cleanup; the result manifest and the
observed timing lines above are the durable evidence.

This closes the slash-normalization defect at the application/wire boundary.
It does not close the separate Qwen3.6-27B concurrent large-object Repo
failure observed in job 181854, and it does not unlock the formal
warmup-plus-five campaign.

## Attempt 181941: post-ACK SDK contract repair and ACK-window amplification — 2026-08-03

Submission `spec162-submission-t009-postack-sdkbinding-20260803T012453Z-001`
(Slurm `181941`) reused the existing 0.6B artifacts, stage manifest, native
file-producer SIF, and Repo registration inputs. It did not rebuild the
foundation image or prepare model bytes. The source bundle was
`e9bda19f853dd76ed2de6db9aabf20f898767e2ee9c72662d4b50636774f5754`; the SIF
was unchanged at
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.

The preceding 181936/181937/181940 failures were caused by Python contract
drift inside the sealed runtime: its installed `ArtifactPreparationMode` enum
contained `GENERATED` and `PRE_SPLIT`, but not `REUSE_CACHED`. The post-ACK
coordinator consequently raised `AttributeError` (`REUSE_CACHED`) before
candidate resolution and artifact preparation. The repair binds the current
`sdk/placement.py` into the SIF at the runtime path; the remote probe returned
`REUSE_CACHED` from that exact module. No image rebuild was needed.

The repaired job proves the intended ordering and content-addressed reuse
path. For token-level requests 0 through 6, the User logged Request first,
then `ACK_CLOSED`, `GRAPH_READY after=ACK_CLOSED`, placement input, decision,
candidate resolution, and Selection. Request 0 emitted
`LLM_PIPELINE_QWEN_DEFERRED_REPO_PUBLISH_DONE`; requests 1 through 5 emitted
`LLM_PIPELINE_QWEN_DEFERRED_REPO_REUSE`, and each received a final NDNSF
Response. The first cold request spent `44898.24 ms` in artifact
resolve/publish; subsequent requests spent `5.32–10.03 ms` in that phase and
did not publish the model again. Providers returned signed ACK metadata with
`cached_shards`, `tier=RELOAD_SAFE_GPU`, matching model/graph/semantics
digests, and cache epochs.

The run also exposed a separate latency defect in ACK closure. The harness
passed `ack_timeout_ms=120000`; although all three Provider ACKs arrived in
about 1–2 seconds, deferred collaboration did not close until the full
120-second window. Each token-level request therefore paid roughly 120 seconds
before the post-ACK strategy could run. The first request measured
`client_request_ms=164929.14` (including the 44.9-second cold Repo
publication), while reuse requests were about `120032 ms`, almost entirely
the ACK window. With one Request per generated token, a 64-token response
would amplify this to approximately two hours. This is not a Repo throughput
finding and not a pre-request deployment barrier.

The run was intentionally cancelled after the useful diagnosis at `00:19:30`;
the requester had produced six valid final Response events and had collected
the ACKs for the next token, but the one-prompt smoke had not yet produced a
complete 64-token answer or `generation-raw.jsonl` record. Per-stage fetch
evidence for the first token requests is retained under:

`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-sdkbinding-20260803T012453Z-001.partial`

The next implementation item is an NDNSF-DI-owned early-close contract: keep
the configured ACK window as a hard upper bound, but close as soon as a
validated DI role-coverage predicate is satisfied. That predicate must remain
outside generic NDNSF and must not permit graph splitting or model preparation
before `ACK_CLOSED`.

## Attempt 181942: complete Qwen3-0.6B response with the post-ACK strategy — 2026-08-03

Submission `spec162-submission-t009-postack-sdkbinding-10s-20260803T014744Z-001`
(Slurm `181942`) reused the same source bundle, native-overlay SIF, stage
manifest, and content-addressed 0.6B artifacts as 181941. It changed only
`SPEC162_ACK_TIMEOUT_MS` to `10000` as a bounded diagnostic; it did not rebuild
the foundation image or prepare model bytes. The source, SIF, and stage
manifest identities remained respectively
`e9bda19f853dd76ed2de6db9aabf20f898767e2ee9c72662d4b50636774f5754`,
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`, and
`8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.

The runtime completed one measured prompt with 47 token-level requests and an
EOS response. `generation-raw.jsonl` reports `status=OK`, `stopReason=EOS`,
`exactReferenceMatch=true`, and a non-empty decoded answer. Every token-level
request emitted `Request -> ACK_CLOSED -> GRAPH_READY -> Selection`, followed
by a final Response; the first request emitted one Repo publish and the next
46 emitted `LLM_PIPELINE_QWEN_DEFERRED_REPO_REUSE`. Each of the three providers
recorded 47 CUDA stage timings, execution-ready markers, Selection preparations,
and dependency-ready markers where applicable. The retained per-stage Repo
progress and completion markers cover the cold artifact load, and later
preparations report GPU-cache hits with zero fetch time.

The Slurm wrapper still recorded `state=FAIL`, `exitCode=1` even though the
User exited 0 and the complete raw response was retained. No `analysis.json` was
produced, so this is runtime-success evidence and not a formal campaign PASS;
the remaining wrapper/analyzer exit path must be diagnosed before this result
can close the gate. The durable partial evidence is:

`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-sdkbinding-10s-20260803T014744Z-001.partial`

This run confirms the intended strategy boundary: application code opens the
Request, ACK closure supplies the strategy input, and only then does the
NDNSF-DI planner consume the ONNX graph and ACK cache/network metadata to
resolve/reuse artifacts and commit Selection. It does not justify treating a
10-second ACK timeout as the architectural solution; the next implementation
item remains a DI-owned validated role-coverage early-close predicate with the
configured timeout retained as a hard upper bound.

## Attempt 181943: atomic marker repair and recovered smoke analysis — 2026-08-03

Submission `spec162-submission-t009-postack-atomicmarker-20260803T020650Z-001`
(Slurm `181943`) reused the same SIF, stage manifest, Repo registration inputs,
and Qwen3-0.6B artifacts. Its only source change was the atomic, newline-
delimited timing-marker writer in `llm_pipeline/user.py`; the new source bundle
identity is `c41e263055c235f950f153b21ef824cc698728da7f55f7a324dfb013bc2cea00`.

The run recorded 47 Request/ACK_CLOSED/GRAPH_READY/Selection/Response cycles,
47 parseable request-phase timing markers, one cold Repo publication and 46
GPU-cache reuses, and the same 47-token EOS answer with
`exactReferenceMatch=true`. The independent analyzer then completed with
`RC=0` and `status=PASS`, covering 141 CUDA stage receipts, 94 dependency
receipts, 47 releases per Provider, and complete cold Repo fetch progress for
all three stages. The recovered analysis is retained as
`analysis-recovered.json`, with its checksum in
`recovered-analysis-sha256.log`, under the partial evidence directory:

`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-atomicmarker-20260803T020650Z-001.partial`

The Slurm wrapper still reports `state=FAIL`, `exitCode=1` before it invokes
its analyzer, so the wrapper result itself is not promoted to a formal PASS.
The retained runtime plus recovered analyzer evidence nevertheless closes the
post-ACK behavior question: the strategy consumes ACK metadata and the ONNX
graph only after `ACK_CLOSED`, resolves or reuses content-addressed artifacts,
and commits Selection. The remaining engineering item is to make the wrapper
preserve this analyzer result and return success; it is independent of model
fetch, Repo reuse, or distributed inference correctness.

## Attempt 181944: official wrapper PASS and complete 0.6B generation — 2026-08-03

The wrapper repair was exercised in one new formal campaign. The SIF, stage
manifest, Repo registration, and Qwen3-0.6B artifacts were reused without
rebuilding or re-preparing the model:

```text
submission: spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001
run:        spec162-run-t009-postack-wrapperfix-20260803T022549Z
Slurm job:  181944 (COMPLETED, exit 0)
source:     d2c3f0637e6f79fc60d4c496d20e5bc7d69b21e2370afa62362ac2c162aae1fc
SIF:        1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368
manifest:   8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5
result:     /project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/
            spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001/result.json
```

The authoritative `result.json` reports `state=PASS` and `exitCode=0`. The
analyzer covered a complete 47-token generation with `stopReason=EOS` and
`exactReferenceMatch=true`; the full decoded response is retained in
`node-0/generation-raw.jsonl`:

```text
NDN 按内容名称转发与 IP 按主机地址转发的核心区别在于：前者基于内容（如文件、消息等）进行转发，而后者基于网络地址（如IP地址）进行转发。
```

The three-stage evidence is:

```text
layer ranges:              [0,9), [9,18), [18,28)
stage receipts:            141 (47 per stage)
dependency receipts:       94
CPU fallback:              0
release markers:           47 per Provider
fetch progress events:     stage0=9, stage1=15, stage2=24
cold Repo fetch (ms):      stage0=8707.04, stage1=19418.62, stage2=27341.51
cold GPU load (ms):        stage0=5522.71, stage1=6479.60, stage2=6977.68
warm GPU cache hits:       46 per Provider (after the first cold request)
```

The progress events are monotonic and the analyzer verified complete segment
coverage. The timing-marker writer now emits one newline-delimited atomic
record, and the wrapper removes the ephemeral shared bootstrap credential
before its secret scan. Those fixes explain why 181942/181943 were retained as
runtime-success diagnostics rather than wrapper PASSes; they do not change the
model, Repo, or distributed-execution path.

This closes the 0.6B TigerCluster infrastructure gate: cold fetch/load,
per-stage progress, dependency-driven execution, complete final response, and
GPU-resident reuse are formally evidenced. Production API granularity remains
one durable full-generation invocation
(`GenerationRequest -> ACK_CLOSED -> Selection -> internal prefill and
autoregressive decode -> Response/ResponseChunk`); the current one-token-per-
collaboration harness remains diagnostic coverage only.
