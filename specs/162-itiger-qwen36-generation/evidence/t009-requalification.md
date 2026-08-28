# T009 Architecture Requalification Evidence

Date: 2026-07-29

Verdict:
`INSTRUMENTED_LOCAL_AND_TIGER_STAGING_PASS__OCI_SIF_AND_LIVE_EVIDENCE_PENDING`

This checkpoint does not complete T009 and does not authorize an OCI registry
push, SIF materialization, model preparation, or distributed inference job.

## 2026-08-01 native-runtime ABI addendum

The later authorized Qwen3-0.6B smoke was retained as a minimal real
three-node gate. Scope-key propagation fixed the first rejection, but jobs
181779, 181781, 181785, and 181787 then aborted in the Repo Provider
constructor with `double free or corruption (!prev)`. Rebuilding the Core,
`ndnsf._ndnsf`, and `py_repoclient._py_repoclient` extensions together did not
remove the failure.

The isolated constructor probe established the cause: the Core and extensions
were built against the host `/usr/local` NDN runtime, but the SIF supplied a
different ABI-compatible-by-name runtime. The mismatch was specifically
observable at `SVSPubSub` construction; replacing all seven dependent runtime
DSOs with the exact build inputs changed the probe outcome to the expected
missing-NFD error and eliminated the invalid free. Therefore the old smoke
results are native-runtime failures, not evidence of a DistributedRepo
storage or model failure.

The immutable remediation is the tracked
`jobs/materialize-core-bindings-fix020.sbatch` job. It seals and hashes the
Core DSO, both Python bindings, and the matching NDN-SVS, NDN-CXX, NAC-ABE,
NDNSD, OpenABE, and Relic DSOs in one SIF. Job 181788 is the first materialized
identity for this repair. A new Qwen3-0.6B smoke is admissible only after its
SIF `result.json`, runtime-library probes, constructor probe, and full
three-node evidence are durable; no earlier failed identity may be reused.

## 2026-08-01 authorized-live addendum

The historical pre-authorization wording above is retained. A later explicit
authorization started a linked TigerCluster requalification against the
prepared Qwen3.6-27B artifact. The manifest digest is
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`,
the model digest is
`sha256:81a39eaabb93592d1d48d051f01607bcecd8f94133f29fcd50b27e340397d1fa`,
and the native runtime SIF is
`/project/tma1/ndnsf-di/releases/spec162-t009-6a0fd98f4c19-a001/runtime.sif`
with SHA-256
`5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5`.
Stage sizes are 18,530,194,174; 15,987,396,002; and 19,274,718,182 bytes.

The immutable live evidence roots are under
`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/`:

| Job / identity | Terminal boundary at this checkpoint |
|---|---|
| 181527 / `...live-001` | FAILED: host campaign path was not translated into the container. |
| 181528 / `...live-002` | FAILED: tokenizer path resolved to `/shared/tokenizer` instead of the bound artifact directory. |
| 181530 / `...live-003` | FAILED: source bundle did not honor the bounded `requireEos=false` smoke contract; provider logs also retained token-mismatch messages during Repo STORE control traffic. |
| 181531 / `...live-004` | FAILED: mixed source revisions caused `run_bounded_qwen_generation()` signature mismatch. |
| 181532 / `...live-005` | FAILED after 01:22:44: first-token Selection rejected with `DI Selection/Core context binding mismatch: provider_boot_epoch`; raw generation retained zero tokens and a 3,600,002 ms request failure. |
| 181535 / `...live-006` | CANCELLED after a global fail-fast experiment made ordinary Repo STORE control selection fail with `provider token mismatch`; the generic path was restored to conservative response waiting. |
| 181536 / `...live-007` | CANCELLED after all three stage registrations and ACKs; the sealed DI diagnostic path raised `NameError: DISelectionAssignmentV2 is not defined` before generation. |
| 181537 / `...live-008` | RUNNING at the latest checkpoint with fix-008 source `sha256:92cea2ceb92d36c08c74cc94924e53ae65a3b15bdfac5ca8075817d91cf68000`; node/routing/Repo/key barriers passed and stages 0–1 registered. |
| 181538 / `...live-009` | CANCELLED: frozen `AckDecision.pending_state_ttl_ms` mutation made all Repo STORE ACKs invalid. |
| 181539 / `...live-011` | CANCELLED after registration/DI assignment success; the installed provider module required a bound source override. |
| 181541 / `...live-012` | CANCELLED after diagnostic logging exposed invalid `DIRoleAssignmentV2.artifact` access. |
| 181543 / `...live-013` | CANCELLED after registration and positive offers: repeated Selection admission attempted a second GPU hold and returned `DI_GPU_CAPACITY_UNAVAILABLE`; no generation claim. |
| 181544 / `...live-014` | RUNNING with fix-014 idempotent reuse of the existing unexpired DI offer. |

All five attempts reached DistributedRepo stage publication before the
user-side failure state. This proves the publication path reached the
large staged artifacts; it does not prove model inference, complete answers,
or repository throughput. In particular, the observed 53.79 GB cold path took
approximately 19–22 minutes to expose the registration record after staging.
Future reports must split payload transfer from root-manifest/catalog ACTIVE
commit and from provider fetch/GPU preparation. The five-prompt formal campaign
remains unexecuted.

## 2026-08-01 Context Mode and control-plane addendum

The repository Context Mode guard temporarily reported
`expected one project ContentDB ... found 0`. The Context Mode doctor still
passed storage, server, FTS5, MCP, and hooks. The failure was caused by the
current mutable `.specify/feature.json` and active Spec 167 authority files
not being present as file-backed sources in the ContentDB. Re-indexing those
five sources restored a PASS with fresh hashes and a current-project SessionDB.
The repair is reproducible with
`scripts/context_mode_index_authority.sh`; it must be run after an active Spec
switch or plan change. This is workflow evidence and does not alter the
immutable TigerCluster evidence roots.

The authorized fix-025/026 Qwen3-0.6B attempts remain pre-inference failures:

| Job | Source identity | Terminal observation |
|---|---|---|
| 181797 | fix-025 coherent runtime | Provider 0 and Provider 2 received the User request; Provider 1 did not. User failed in the ACK window. |
| 181798 | fix-026 plus explicit User producer route | Only Provider 0 received the User request. The route addition alone did not establish remote SVS convergence. |

Neither job reached Selection, Repo model fetch, GPU preparation, or token
generation. A comparison with the successful Spec 160 three-node harness found
that Spec 160 waited 120 seconds after Provider readiness, while Spec 162 had
regressed to 10 seconds. The next source bundle restores a configurable
120-second Provider settle barrier and a 5-second User startup settle. These
are control-plane readiness gates, not performance tuning or evidence of a
successful inference.

The follow-up source correction staged for the next live run is fix-009,
source manifest SHA-256
`ec8212dbe219f492d0e4474837bdb0208fdf2a079ab42c2409a0c0393e65850d`.
`network_artifact_backend.py` now derives a bounded provider ACK
`pending_state_ttl_ms` from artifact size (8 MiB/s conservative estimate plus
five minutes, capped at one hour). This protects the selected request and its
one-time ProviderToken while Repo performs long pull/verify/finalize work; it
does not reserve storage capacity or change the no-lock Repo queue contract.

Fix-010 through fix-014 are retained as separate source-bundle identities:
they bind the current DI provider module, construct the frozen ACK with its
TTL, log the original preparation exception, and use the valid Repo
registration `objectName` cache key, respectively. Their source-manifest
hashes are `78cbc27e6d77f31ceca8c5876fd6054a4b9f922d67a721c91c0cdab6c2898b21`,
`a01801b57d456cdb28862e8750efaa7aa8ae9861815021c9fa4f8ec4b8022868`,
`328c1cbe309b9574fc68019ab4c7208eb2481a6e08048828a4edbebd37f8930e`, and
`344efb1ba8b70b3fdc27182528d3eb39eeb300569a992c0a7c50f0294abbdf95`, and
`54277995fc0392d37aa37b6e49cea785749670252abc916c3f87ed81ac5b07a6`.

Job 181532's retained `result.json` is `FAIL`, and its single raw generation row
has `stopReason=REQUEST_FAILURE`, zero generated tokens, and no inference
result. The user received all three ACK offers and Repo registration completed,
but every first-token Selection was rejected by the Core/DI
`provider_boot_epoch` binding check.

The root cause is visible in the immutable fix-004 source: its
`llm_pipeline/provider.py` constructed the DI offer issuer, GPU ledger, and
Selection participant with the synthetic `--provider-boot-epoch` argument.
The native Core context uses the Provider process-incarnation epoch exposed by
`ServiceProvider::getProviderBootEpoch()`. These values are different by
design, so the fail-closed rejection is correct. The source fix must obtain the
epoch from `APPProvider.provider_boot_epoch`, use it for every Selection
artifact, and reject any conflicting CLI value at startup.

## Frozen model and placement

- Model: `Qwen/Qwen3.6-27B`
- Revision: `6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`
- Precision: BF16, no quantization
- Generation: greedy, `use_cache=false`, no thinking, at most 64 new tokens
- Layer ranges: `[0,21)`, `[21,42)`, `[42,64)`
- Target: three distinct RTX 5000 nodes, one stage per node

## Candidate identities

| Object | Identity |
|---|---|
| Qwen overlay lock | `sha256:aedbff59b78a23f2288b8228d18a9d45dee7194cf13e5fa6d19b02311c2dc5c4` |
| Application lock | `sha256:47a177da1b82f665b8f92dec2477c8ac1768b5bcfa2edf2147a0f523e47acae6` |
| Application source seal | `sha256:676ee3283420339c694ab3a1ef8457740cd01435e9de343566a3738045a486f7` |
| Application parent image ID | `sha256:116ffc601e0b90762ef4101ca941bea3765c7a528561e248c515090f971d3c0a` |
| Qwen runtime base image ID | `sha256:eaf7c064c0fc5011f6a6e3502903d15636694e7d8d523591d80c366b5d1ed819` |
| Instrumentation source seal | `sha256:be65136eaa7e27bfc37b4c289fed78440b5dfc54c891d0f89466f019d323dbec` |
| Current final local Docker image ID | `sha256:2b4ee488859ffaca908e578f3d60d44579b96f27d48fe19ae4160b39bab28209` |
| Current compressed Docker archive SHA-256 | `8e233c60438e0d10047fc0e45ee731a6ec4e9b668b21eb7575870cc4746d13e7` |
| Current normalized source-bundle SHA-256 | `b7806693a073989c492056baa699988d7d6034e9e499fda236ea7a83f7e14a17` |

The Docker image ID is a local image-configuration identity. It is not an OCI
manifest digest and must not be reported as one.

The previously staged `eaf7...` source/archive pair is retained read-only as
`SUPERSEDED_PRE_INSTRUMENTATION`; it must not be submitted. The current
candidate is staged read-only at:

- Source bundle:
  `/project/tma1/ndnsf-di/sources/spec162-t009-2b4ee488859f`
- Docker archive:
  `/project/tma1/ndnsf-di/images/spec162-2b4ee488859f/candidate-docker-archive.tar.gz`
- Archive size: `4,429,282,926` bytes
- Local and Tiger `gzip -t`: PASS
- Local and Tiger full-file SHA-256: PASS
- Tiger archive mode: `0400`; source directory mode: `0500`; no staged path
  has an owner, group, or other write bit
- Tiger source file count: 31
- Tiger path-and-content manifest SHA-256:
  `7ea327327aa5462ec3fe83d6235ba96c4d9f1c8b44a5a46d0a02ee5e204b4e21`
- Tiger source-seal verification: 30/30 non-image entries PASS, with seal
  `sha256:be65136eaa7e27bfc37b4c289fed78440b5dfc54c891d0f89466f019d323dbec`
- Tiger archive member count: 101; single-image Docker manifest: PASS
- Archive contains final image configuration
  `2b4ee488859ffaca908e578f3d60d44579b96f27d48fe19ae4160b39bab28209`

## Passed gates

- Spec 163 audit:
  `PASS WITH EXPLICIT NO-CUDA DEFERRAL`
- Spec 163 local Docker gate: PASS
- Spec 163 MiniNDN evidence: 59/59 rows and 23/23 gates PASS
- Focused DI tests: 14 files, 86 tests PASS
- Seal tests: 8 PASS
- Qwen overlay tests: 4 PASS
- Clean owner-wheel installation-profile test: PASS
- Fresh application layered build
  `build-spec162-t009-20260729c`: PASS in 667.255 seconds
- Final image runtime versions:
  Transformers 5.14.1, tokenizers 0.22.2, torch 2.6.0+cu124
- Final image banned-path scan: `bannedPathCount=0`
- Final image runtime API probe: PASS
- Final image local fake collaboration and operation-status smoke:
  `results/spec162-itiger-qwen36-generation/local-docker-operation-status-20260729T124000Z`
- Instrumentation overlay build:
  `results/spec162-itiger-qwen36-generation/build-spec162-t009-instrumented-20260729a`
- Instrumentation overlay delta: 134,410 bytes; no model weights
- ACK/release analysis now distinguishes complete application generations
  from token-level NDNSF Requests and requires, on every Provider,
  `release(request N) < ACK=true(request N+1)`

No model weights were embedded in the image.

## Pre-authorization SIF tooling

The historical SIF jobs remain bound to OCI `6216ef2b9525...` and are not
edited or reused. T009 now has a separate fail-closed rendering path:

- `jobs/render-t009-sif-jobs.py`
- `jobs/materialize-qwen36-runtime-t009.sbatch.in`
- `jobs/operation-status-sif-smoke-t009.sbatch.in`

The renderer accepts only the raw, single-image OCI manifest whose byte digest
matches the supplied immutable GHCR reference and whose config digest equals
the current frozen local image ID
`sha256:2b4ee488859ffaca908e578f3d60d44579b96f27d48fe19ae4160b39bab28209`.
It rejects tags, another repository, manifest-digest mismatch, config-image
mismatch, malformed layer descriptors, unresolved template tokens, and a
nonempty output directory. It emits:

1. the byte-identical OCI manifest;
2. a candidate-bound CPU SIF materialization job;
3. a separately authorized RTX 5000 fake-collaboration smoke job;
4. the required smoke harness;
5. a submission plan containing exact identities, script hashes, evidence
   paths, and commands.

Five renderer unit tests and `bash -n` for both rendered jobs pass. No rendered
submission plan exists yet because no real OCI manifest digest exists.

The renderer copy inside the immutable staged source bundle is archival. It
predates the final `2b4e...` staging constants and MUST NOT be executed. After
publication, render from the current repository copy of
`jobs/render-t009-sif-jobs.py`, bind its bytes and outputs in the generated
submission plan, and stage that plan separately. This restriction does not
affect the measurement runtime files covered by the source seal.

The formal analyzer has an independently sealed v3 analysis contract at
`evidence/t009-formal-analysis-contract.json`. V3 requires all 13 requester
timing phases on every token Request, all 13 Provider stage timing phases on
every rank and Request, CUDA-only execution, zero artificial delay, and the
direct log order
`ACK_TRUE < STAGE_TIMING < RESERVATION_RELEASED < NEXT_ACK_TRUE`. It reports
the first cold Request and complete generation separately, plus requester,
Provider preparation, and Provider execution distributions over all subsequent
token Requests. The analyzer and contract must be staged read-only as a
separate analysis bundle; the older analyzer snapshot in the immutable
measurement bundle is archival.

Tiger read-only analysis staging:

- Directory:
  `/project/tma1/ndnsf-di/analysis/spec162-t009-v3-7030ec6d2b28`
- Analyzer SHA-256:
  `7030ec6d2b289f9b45e4fa4d31af78133f90aec83947d763f81ad241e4ffd0bb`
- Contract SHA-256:
  `da9fc1ac8b7b968174bbbd7bb1154503ca3270325145eaa8476fc0e6574d8e9e`
- Remote hash/schema/no-write-bit verification: PASS

## Preserved failures and corrections

1. `build-spec162-t009-20260729a` failed because the working-tree source seal
   omitted an allowlisted untracked header. The seal now includes exact
   allowlisted untracked paths while excluding build output and keys.
2. `build-spec162-t009-20260729b` failed because no wheel owned the base adapter
   modules. The SDK wheel now owns the base adapter package; the clean
   owner-wheel test verifies collision freedom and runtime import.
3. The first archive upload stopped after `268,140,544` bytes because local
   Docker archive creation temporarily consumed constrained root storage. The
   incomplete remote object was removed, targeted builder cache was pruned, and
   the complete archive was uploaded and verified.
4. Historical preparation Job 175053 remains FAILED 1:0 due to missing
   `PYTHONPATH`; it is not rerun or reclassified.

## Remaining T009 gates

1. Obtain explicit authorization before publishing the current candidate to
   GHCR.
2. Record the resulting immutable OCI manifest and digest, then run the
   fail-closed T009 job renderer.
3. Stage the rendered plan read-only, verify its hashes, and obtain separate
   explicit authorization before submitting its SIF Slurm job.
4. Verify the built SIF with the bounded fake-collaboration gate.
5. Under a new explicit live authorization, prepare the three model stages,
   publish them through `NDNSF-DistributedRepo`, and retain all root-manifest
   receipts.
6. Verify exact provider Repo fetch, CUDA load, persistent GPU residency, warm
   reuse, and reservation release before starting the formal campaign.

### 2026-08-01 Fix-027 result: readiness reached, Stage 2 absent

Job `181799` (`spec162-submission-t009-qwen3small-smoke-fix027-173631`, run
`spec162-run-t009-qwen3small-smoke-fix027-20260801T173631Z`) used the new
source bundle SHA-256
`8063bf6b9b57bb8c4fb61684b44ecf3e254d4a57a29852ea240962c672670286`, the
fix-025 SIF SHA-256
`f3da4d2147ab66e125a0695ea4fa88920f7dec7da20e7f9a0e88ce2d0a8cc7ce`, and
stage-manifest SHA-256
`ef9df35d362da17f61eaf5edd9dead5be8b83523ddfa408d8fd6a42fc5b694fe`.

The 120-second Provider settle and 5-second User startup changes removed the
earlier “request never reached remote Providers” boundary. This job recorded
all three Provider-ready markers, Repo registration-ready, all three
selection-residency records, and a User request after startup. Provider 0 and
Provider 1 returned valid `DI_SELECTION_DATAFLOW_V2_READY` ACKs. Provider 2
started with `/LLM/Pipeline/Stage/2` but emitted no request/ACK event. The
User failed with:

```text
no capacity-safe split/provider placement: ...:no feasible Provider for /LLM/Pipeline/Stage/2
```

The candidate planning manifest requires `2877`, `2550`, and `2910` MiB for
Stages 0–2 respectively. Because Stage 2 had no feasible participant, the
job ended before Selection, model fetch, GPU preparation, or token generation
(`result.json` state `FAIL`, exit code 1). This is a control-plane Stage-2
participation/readiness failure; it is not a DistributedRepo throughput
measurement. The immutable partial evidence is retained at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix027-173631.partial`.

Next, run a bounded Stage-2 producer/request probe using a new identity and
the same sealed inputs. Only after all three ACKs are observable should the
generation gate be repeated.

### 2026-08-01 Fix-028 result: longer User settle did not restore coverage

Job `181800` (`spec162-submission-t009-qwen3small-smoke-fix028-175152`, run
`spec162-run-t009-qwen3small-smoke-fix028-20260801T175152Z`) reused the exact
fix-027 source/SIF/manifest identities and changed only
`SPEC162_USER_STARTUP_SETTLE_MS=60000` (Provider settle remained 120 seconds).
All three Provider-ready markers and Repo/residency barriers completed. The
User request was published after the 60-second settle, but only Provider 0
returned a valid ACK; Providers 1 and 2 logged no request/ACK. The User failed
before Selection with:

```text
no capacity-safe split/provider placement: ...:no feasible Provider for /LLM/Pipeline/Stage/1
```

The negative control rejects the hypothesis that the prior 5-second User
settle alone caused the missing remote participants. Both fix-027 and fix-028
remain pre-Selection, zero-token failures and are not Repo throughput or GPU
generation measurements. Preserve Job 181800 at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix028-175152.partial`.
The next probe must capture and verify User certificate Interest routing and
SVS mapping delivery on each node before changing placement or memory policy.

### Fix-029: all three ACKs, then offer expiry at the Selection boundary

Job 181801 (`spec162-submission-t009-qwen3small-smoke-fix029-181103`) used
source-manifest SHA-256
`365ac8bf4c6e4473c57d7c01518d8d7f62076eec4a4c5d93f2f67707ce9d4c64`, the
unchanged fix-025 SIF, and the unchanged Qwen3-0.6B stage manifest. The
120-second ACK window and 60-second User settle produced the first complete
control-plane coverage: Providers 0, 1, and 2 each logged the User request
and a true `DI_SELECTION_DATAFLOW_V2_READY` ACK with the expected Stage 0/1/2
role.

Selection was not accepted. Provider validation reported
`LLM_PIPELINE_QWEN_SELECTION_VALIDATION_FAILED ... offer_expiry(delta_ms=-8)`.
The timeout-driven User strategy waited through the 120-second ACK window,
which exactly consumed the 120-second offer lease. No Selection commit,
DistributedRepo fetch, GPU load, or generated token is attributable to this
run. The live logs were copied into the fix-029 partial evidence before the
allocation was cancelled; this is a preserved FAILED control-plane result.

### Fix-030: preparation passed; scope-key sealing failed

Job 181802 (`spec162-submission-t009-qwen3small-smoke-fix030-182404`) uses
source-manifest SHA-256
`a52af657f5b56c4c7f5ae5f0d8e76e63d2c20f852659bd80db74f8a1ae631f9d` and
sets `SPEC162_ACK_TIMEOUT_MS=120000` with
`SPEC162_SELECTION_OFFER_LEASE_MS=600000`. It reached Selection with all
three valid ACKs. DistributedRepo fetched all three content-addressed stage
artifacts (594,357,850; 283,192,711; and 625,825,930 bytes), and all Providers
loaded their assigned stage on CUDA with `cpuFallback=false`. Stage 0 executed
and published an 81,816-byte activation. No final response was produced.

The failure is deterministic and attributable to the automatic-planning
coordinator: `commit_plan` carried the symbolic dependency scopes but an empty
`scope_key_data_names` map. Provider 0 therefore emitted `Missing
collaboration scope key ... scope=pipeline-stage-0-to-1`; Providers 1 and 2
timed out waiting for their dependency references. The raw logs are preserved
in the Fix-030 partial evidence directory. Repo fetch, model loading, CUDA
execution, and persistent-residency evidence are positive sub-gates, but the
end-to-end Qwen gate remains FAILED.

Fix-031 changes only the trusted plan-sealing path: it publishes an encrypted
32-byte key for every dependency scope after placement, validates the returned
absolute Data name, and commits those names before Selection. It uses a new
content-addressed source bundle and request identity; no model artifacts are
recopied.

### Fix-031 terminal evidence: readiness regression before Selection

Job 181804 (`spec162-submission-t009-qwen3small-smoke-fix031-184255`, run
`spec162-run-t009-qwen3small-smoke-fix031-20260801T184255Z`) used source SHA
`aeccac1ffcd6b0c7765fd513bae1beea344067e3809edf811d1e3b62e13f2019`, the
unchanged SIF and stage manifest, and the new scope-key sealing code. Repo
registration completed and all three Providers became ready, but the first
token request reached only Provider 0. The User received one ACK and timed out
with `no feasible Provider for /LLM/Pipeline/Stage/1`; Providers 1 and 2
logged no request/ACK. The request therefore never reached Selection, scope-key
publication, Repo fetch, CUDA preparation, or generation. This is preserved as
a pre-Selection control-plane negative result and does not invalidate the
Fix-030 Repo/CUDA evidence or test the Fix-031 scope-key change.

The next attempt keeps the same content-addressed source and model inputs but
uses a new submission identity and a longer Provider convergence barrier before
the User starts. It must again observe three role-specific ACKs before any
scope-key or generation conclusion is drawn.

### Fix-032 terminal evidence: requested settle was not propagated

Job `181805` (`spec162-submission-t009-qwen3small-smoke-fix032-185658`, run
`spec162-run-t009-qwen3small-smoke-fix032-20260801T185658Z`) reused the Fix-031
source SHA `aeccac1ffcd6b0c7765fd513bae1beea344067e3809edf811d1e3b62e13f2019`,
the unchanged SIF, and the unchanged Qwen3-0.6B stage manifest. The submission
requested a 300-second Provider settle, but `generation-rank.sh` did not pass
`SPEC162_PROVIDER_SETTLE_SECONDS` through Apptainer, so the inner wrapper used
its 120-second default. This is a launcher evidence defect, not a measured
300-second readiness result.

All three Provider-ready and Repo-registration barriers completed. The first
token request was processed only by Provider 0; the User received one valid
ACK and failed before Selection with
`no feasible Provider for /LLM/Pipeline/Stage/1` after 120008 ms. Providers 1
and 2 emitted no request/ACK. Consequently this run has no scope-key, Repo,
CUDA, or generation evidence. The terminal `result.json` is retained in
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix032-185658.partial`.

The outer launcher now validates and propagates the settle variable with the
inner script's 30–900 second bounds. Fix-033 is the first valid test of the
longer barrier and must use a fresh immutable source/submission identity.

### Fix-033 terminal evidence: 300-second barrier was effective, but remote faces stayed idle

Job `181806` (`spec162-submission-t009-qwen3small-smoke-fix033-190915`, run
`spec162-run-t009-qwen3small-smoke-fix033-20260801T190915Z`) used source SHA
`3ff785c7fb923073578bbc52ad0f244a6618a148fae58a3734d62c5084078f3f` and the
corrected launcher. The remote process tree records the inner `sleep 300`, so
the requested Provider convergence barrier was actually applied.

The three Provider-ready and Repo-registration barriers completed. After the
User started, only Provider 0 logged the first token request and ACK; Providers
1 and 2 logged no request/ACK. The User failed before Selection with
`no feasible Provider for /LLM/Pipeline/Stage/1` after 120008 ms. The immutable
`result.json` is `FAIL` (exit 1) at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix033-190915.partial/result.json`.

NFD captured the expected aggregate route, remote nexthops, and multicast
strategy, but the remote faces had zero packet counters. Thus Fix-033 is a
pre-Selection control-plane/native request-forwarding failure. It provides no
scope-key, DistributedRepo fetch, CUDA, or generation evidence and must not be
used to attribute the delay to Repo throughput. The next run is blocked on a
targeted native `beginCollaboration`/request-publication probe that proves why
the remote faces do not transmit.

### MiniNDN-first recheck before another Tiger submission

The same three-stage fake workload was first run through
`Experiments/NDNSF_DI_LlmPipeline_Minindn.py`. The initial QuickChecks wrapper
failed before the User request because it omitted the script's explicit
`--test-only-allow-ephemeral-app-state` acknowledgement and the client rejected
the volatile `/tmp` journal root. The wrapper now supplies that test-only flag;
this is a harness correction and does not alter durable application behavior.

The corrected run passed with MiniNDN NLSR (`LLM_PIPELINE_MININDN_OK`, warmup
305.07 ms, measured 133.48 ms) and with the script's
`--static-routing-only` mode (`LLM_PIPELINE_MININDN_OK`, warmup 346.56 ms,
measured 154.43 ms). Both runs selected all three stage roles, published the
request-scoped scope keys, and returned a three-stage response. This local
evidence rules out a generic `BeginCollaboration`/`PublishRequestV2` failure.
Fix-033 remains a Tiger-specific route/face or SVS control-plane regression;
the next remote test must capture the group-prefix FIB and face counters during
the actual User publication before any NDNSF internal change.

### Fix-034b terminal evidence: User mapping publication raced remote certificate convergence

Job `181807` (`spec162-submission-t009-qwen3small-smoke-fix034b-194332`, run
`spec162-run-t009-qwen3small-smoke-fix034b-20260801T194332Z`) used source SHA
`5c246622489349d1dbd27c622f503c389471b6b976c57ae78938ff0619737670`.
The corrected Tiger script installed the exact
`/NDNSF-DistributeInference/example/group` route and multicast strategy on
each rank. The 300-second Provider settle and 60-second User startup settle
were effective; all NFD, Controller, Repo, selection-key, and Provider-ready
barriers completed.

Only Provider 0 processed the User request and returned an ACK. Providers 1/2
had no request/ACK, and the User failed before Selection after 120008 ms. Their
logs show repeated group-SVS Data validation and an initial missing User/remote
Provider certificate; the User producer mapping was not fetched after the
certificate became available. No scope-key, Repo fetch, CUDA, or generation
evidence is attributable to this run. Preserve the terminal result at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-smoke-fix034b-194332.partial/result.json`.

This result keeps the fault boundary in Tiger's manual TCP/SVS startup and
identity convergence. MiniNDN NLSR and static-routing controls both pass the
same collaboration path, so no `ServiceUser` internal rewrite is justified.
Fix-035b tests a 240-second User startup settle with unchanged source/model
artifacts.
