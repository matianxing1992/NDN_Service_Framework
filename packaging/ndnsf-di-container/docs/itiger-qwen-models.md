# iTiger NDNSF-DI Qwen operations runbook

This is the practical entry point for running Qwen experiments with NDNSF-DI
on iTiger. Repository scripts, frozen source bundles, and promoted evidence are
authoritative. Personal wrappers may shorten commands, but never own a run
identity, model, runtime, or evidence.

## What each test layer proves

Run the cheapest relevant gate first. Do not use a later, expensive Qwen job to
discover a failure that a smaller gate could have found.

| Gate | Recommended workload | What it proves | What it does not prove |
|---|---|---|---|
| Local contract tests | Python/unit fixtures | generation state, EOS, request correlation, analyzer rules | native bindings, NFD, CUDA |
| Local Docker security smoke | one container; controller, NFD, one provider, one user; tiny fake payload; `--memory=4g --memory-swap=5g` | collaboration assignment, permissions/tokens, and `report_operation_status()` in the candidate image | SIF materialization, cluster networking, GPU/model execution |
| Single-node Slurm SIF smoke | same fake workload in Apptainer | the sealed SIF works on a compute node without mounted replacement libraries | cross-node NFD or Qwen correctness |
| Multi-node NFD probe | bounded allocation, no model | allocated nodes can exchange the required signed named data over job-scoped TCP/UDP faces | CUDA or model execution |
| Standalone model reference | one GPU, frozen revision and prompt contract | expected tokens, tokenizer/chat template, capacity | NDNSF-DI execution |
| Stage load smoke | one GPU per stage | exact layer coverage, artifact integrity, CUDA load, no CPU fallback | cross-node dependency flow |
| NDNSF-DI development smoke | three nodes, one prompt/request | secured assignment, all stages, two dependency transfers, final exact result | performance distribution or production readiness |
| Formal generation campaign | frozen prompts and repetitions | full EOS-terminated answers and retained latency distribution | KV-cache, throughput scaling, tensor parallelism unless separately designed |

The local Docker smoke is intentionally small. Do not load Torch/Qwen or start
three model providers on a memory-constrained workstation merely to test the
security/control path. The accepted Spec 160 harness is
`specs/160-itiger-multinode-qwen-collaboration/jobs/run-local-docker-operation-status-smoke.sh`.

## End-to-end workflow

### 1. Freeze the experiment before live work

Record and checksum:

- feature/spec and task identity;
- immutable 40-hex model revision;
- tokenizer and chat-template policy;
- dtype, layer ranges, prompt set, decoding/EOS policy, and token ceiling;
- source bundle and coherent SIF;
- Slurm resources and physical placement requirement;
- unique submission/run identity and any explicitly replaced failed identity;
- expected evidence schema and analyzer version.

Rendering or copying a job is not submission. A formal live identity is
submitted exactly once. Never silently edit and reuse an identity after it has
started.

### 2. Discover current cluster facts

Connect the VPN, then perform read-only discovery:

```bash
uofm-vpn-status
ssh -o BatchMode=yes itiger 'hostname; whoami'
discover-itiger.sh
discover-qwen-readiness.sh
```

Repository-level Qwen discovery is also available:

```bash
tools/ndnsf-di/ndnsf-di-qwen discover \
  --host itiger --output results/itiger-qwen/discovery
```

Discovery is not permission to download, build, or compute. Never run model
preparation or inference on the login node.

### 3. Separate durable capacity from temporary peak capacity

Use project storage only for promoted, immutable outputs:

```text
/project/$USER/ndnsf-di/releases     coherent SIF releases
/project/$USER/ndnsf-di/models       sealed stage/tokenizer artifacts
/project/$USER/ndnsf-di/manifests    immutable registries and digests
/project/$USER/ndnsf-di/evidence     promoted evidence
```

Select job scratch only inside the allocation, in this order:

1. `$SLURM_TMPDIR`;
2. `/scratch`;
3. `/tmp`.

The job must prove the selected path is writable, allocation-scoped, and large
enough. Shared `df` output is not a user quota and an advertised `/scratch`
path is not evidence that it exists on the allocated node.

Calculate two independent gates:

```text
temporary peak =
  source download + cache + reference model + stage construction + margin

durable peak =
  promoted stages + tokenizer + manifests + evidence + required reserve
```

Do not keep a reconstructible full source-model copy or download cache in
project storage when the experiment only needs promoted stages. Cleanup may
remove only the current job's validated scratch prefix after checksums,
per-stage CUDA loads, promotion bytes, and durable reserve all pass.

### 4. Transfer and seal a reusable source only when required

Some scaling experiments require a durable sealed source model; the current
32B preparation does not and promotes only stages/tokenizer/evidence. When the
spec explicitly requires a reusable source, render first and submit once only
after inspection:

```bash
tools/ndnsf-di/ndnsf-di-qwen transfer \
  --model-entry model-entry.json --run-id qwen-transfer-v1 \
  --output job.sbatch --ledger submission-ledger.json
# Inspect the rendered job and preflight evidence before using --submit.
```

The transfer must run on a bounded CPU Slurm allocation, pin a 40-hex immutable
revision, retain upstream file sizes and SHA-256 digests, quarantine partial
downloads, and promote only a complete manifest. Never accept a floating
branch or an LFS pointer as a sealed model.

The accepted historical 0.5B source is `Qwen/Qwen2.5-0.5B-Instruct` revision
`7ae557604adf67be50417f59c2c2f167def9a775` (transfer Job 146050, seal Job
146123). Those jobs were not replaced.

Record the model-card license before transfer. Do not infer one Qwen size's
license from another: Qwen2.5 3B and 72B require their own review rather than
the Apache-2.0 classification used by most other sizes.

### 5. Build one coherent runtime

The SIF must contain a mutually compatible native stack: NFD/NDNSF libraries,
Python bindings, PyTorch/Transformers, CUDA user-space libraries, and the
application code. The host supplies the NVIDIA driver and allocated devices.

Do not combine an older SIF with mounted replacement `.so` files, pybind
modules, or a different vendor site-packages directory. Spec 160 showed that
this can produce constructor/import mismatches and allocator corruption before
model execution. A local Docker pass alone does not prove that the materialized
SIF is coherent, so repeat the small operation-status smoke inside a bounded
Slurm allocation.

GPU validation fails closed. A visible GPU is insufficient: every stage must
record its allocated GPU UUID, `device=cuda:*`, a successful CUDA operation,
and `cpuFallback=0`.

### 6. Prove cross-node NFD before loading Qwen

For a multi-node candidate, create job-scoped NFD configurations from the
allocated hostnames and addresses. Run the bounded TCP/UDP probe first and
retain face configuration, node mapping, Data names, and results.

Do not treat login-node connectivity, ICMP reachability, or a single-node NFD
smoke as a substitute. If this probe fails, stop before model preload and
classify the result as a network/runtime preflight failure, not a Qwen failure.

The accepted Spec 160 reference harness is:

```text
specs/160-itiger-multinode-qwen-collaboration/jobs/nfd-multinode-probe.sbatch
```

### 7. Prepare the reference and stages

Preparation runs under Slurm:

1. verify the frozen capacity decision and source/SIF digests;
2. download the exact revision into current-job scratch;
3. generate deterministic standalone reference tokens with the frozen
   tokenizer, chat template, dtype, and decoding policy;
4. construct complete, non-overlapping layer-range packages;
5. checksum each package and load it on CUDA with CPU fallback forbidden;
6. promote only the accepted tokenizer, stages, policy, manifests, reference,
   and evidence;
7. verify promoted bytes and reserve;
8. remove only the validated scratch prefix owned by this job.

For the current 32B generation feature, see:

```text
specs/161-itiger-qwen32b-generation/quickstart.md
specs/161-itiger-qwen32b-generation/jobs/prepare-reference.sbatch
specs/161-itiger-qwen32b-generation/jobs/prepare-stages.sbatch
```

### 8. Run one development smoke before a campaign

Use three distinct allocated nodes and GPU UUIDs. The smoke must correlate:

- outer submission/run identity and indexed application request ID;
- request/session name, role, provider, host, GPU UUID, and layer range;
- three CUDA stage receipts;
- two producer-to-consumer dependency receipts with matching names, bytes,
  segment counts, and SHA-256 digests;
- final requester response and exact reference token sequence;
- operation-status snapshots and terminal status.

For autoregressive generation, each token epoch must repeat this correlation.
The requester appends the accepted token to the next full context and stops on
EOS or the frozen ceiling. A top-token-only run proves a single forward pass;
it does not prove that NDNSF-DI generates a complete answer.

Current 32B development-smoke harness:

```text
specs/161-itiger-qwen32b-generation/jobs/generation-smoke.sbatch
specs/161-itiger-qwen32b-generation/jobs/analyze-generation-smoke.py
```

### 9. Run the formal campaign and preserve the distribution

Freeze warmup, prompt, and repetition counts before submission. Spec 161 uses
five prompts, one excluded warmup, five measured generations per prompt, greedy
decoding, EOS, and a 64-generated-token ceiling.

Retain raw per-token and per-generation records. Summaries must exclude warmup,
failed, mismatched, cancelled, timed-out, and truncated rows without deleting
them. Report per-prompt and pooled descriptive distributions; do not claim p99
from an inadequate sample or mix prompts without labeling the pooled result.

The current full-context `use_cache=False` path is a correctness/distribution
baseline. It is not evidence for distributed KV-cache, tensor parallelism,
throughput scaling, or a performance improvement.

## Queue and status handling

`PENDING (Resources)` is normal and does not justify a duplicate submission.
Read status without changing the job:

```bash
squeue -j JOB_ID -o '%.18i %.9P %.28j %.8T %.10M %.20S %R'
scontrol show job JOB_ID
sacct -j JOB_ID --format=JobID,State,ExitCode,Elapsed,NodeList
```

Do not cancel, resubmit, or modify a queued exactly-once job merely because the
estimated start moves. Submit a linked replacement only after a terminal
failure, root-cause classification, corrected frozen inputs, and explicit
authorization.

## Frequent failures and the correct response

| Symptom | Likely class | Correct response |
|---|---|---|
| `double free`, invalid pointer, native import/constructor mismatch | mixed runtime | stop; rebuild and seal one coherent SIF; do not mount replacement native libraries |
| `report_operation_status()` missing or binding mismatch | Python/native API drift | reproduce with the tiny Docker smoke, rebuild the app/binding layer, then repeat the single-node SIF smoke |
| cross-node request timeout before model load | NFD face/routing/configuration | run the allocation-scoped NFD probe; keep it separate from model evidence |
| CUDA visible but stage reports CPU | backend fallback | fail the cell; retain logs; never accept timing or correctness from that cell |
| model download fills project storage | source/cache placed on durable storage | use measured allocation scratch and promote only final stages/tokenizer/evidence |
| `PENDING (Resources)` for hours | scheduler contention | wait and inspect; do not create a duplicate identity |
| workload succeeds but final analyzer exits nonzero | evidence-tooling failure | retain the formal job failure; audit immutable workload evidence read-only; never relabel the scheduler state |
| top token matches once | bounded correctness only | report one-forward capability; run EOS generation and repetitions for complete-answer/distribution claims |

## Cleanup

Cleanup begins with a content/reference-aware dry run:

```bash
tools/ndnsf-di/ndnsf-di-qwen cleanup \
  --candidates cleanup-candidates.json --protected protected.json \
  --output cleanup-plan.json --dry-run
```

Protect current/prior releases, active jobs, source/candidate identities,
accepted and failed formal evidence, referenced models, and every evidence
root. Delete only explicitly reviewed, unreferenced diagnostic or cache
content. Never use cleanup to hide a failed measured attempt.

## TigerCluster Qwen3.6-27B lessons — 2026-08-01

The first explicitly authorized Qwen3.6-27B requalification used the real
three-node NDNSF-DI path and the prepared content-addressed stage manifest
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`.
The three stage payloads total 53,792,308,358 bytes. The native SIF was
`/project/tma1/ndnsf-di/releases/spec162-t009-6a0fd98f4c19-a001/runtime.sif`
with SHA-256
`5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5`.

Use these rules when rendering the next identity:

1. Bind the campaign manifest to `/shared/<basename>` inside every container.
   Passing a host `/project/...` path directly makes the user fail before
   inference even when Repo publication succeeds.
2. Resolve the tokenizer from the bound model artifact directory
   (`$SPEC162_ARTIFACT_DIR/tokenizer`), not an assumed `/shared/tokenizer`.
3. Treat `user.py` and `llm_pipeline_lib.py` as one sealed compatibility unit.
   Mixing a newer requester file with an older library produced a keyword
   signature failure before the first token.
4. Keep the bounded smoke stop policy (`requireEos=false`) in the sealed source
   contract. A legacy user that unconditionally requires reference EOS is not a
   valid 64-token smoke.
5. Use the Repo registration record and root-manifest receipts as the boundary
   between “bytes staged” and “catalog ACTIVE”. Do not infer readiness from
   stage files or directory size.
6. Record transfer, registration/ACTIVE commit, Provider fetch/cache, GPU
   preparation, and inference separately. The observed 53.79 GB cold path took
   roughly 19–22 minutes before registration became visible; this is not a
   Repo throughput result.
7. Preserve content-addressed stage digests and Provider GPU residency for a
   later warm-request measurement. The first cold request and later cache-hit
   requests must never be pooled into one latency number.
8. Keep Provider-token mismatch messages visible in the control-plane ledger.
   Live-003 emitted such messages during Repo STORE selections while
   publication still completed; this is a security/correlation diagnostic,
   not evidence that the final inference path is authorized.

The linked identities were Jobs 181527 (`live-001`), 181528 (`live-002`),
181530 (`live-003`), 181531 (`live-004`), and 181532 (`live-005`). All five
are preserved failures with pre-generation causes; live-005 specifically
rejected the first Selection because a synthetic CLI boot epoch differed from
the native Core epoch. See
`specs/162-itiger-qwen36-generation/evidence/t009-requalification.md` and
`docs/NDNSFDI/tigercluster-qwen36-operational-lessons.md` for evidence paths.

### Native runtime ABI incident — 2026-08-01

The minimal Qwen3-0.6B TigerCluster smoke exposed a deployment-image issue
before inference: Core-only, Core-plus-DI-binding, and Core-plus-both-binding
images all aborted in `SVSPubSub` with `double free or corruption (!prev)`.
The isolated constructor probe reproduced the failure without model bytes,
Repo traffic, or CUDA. The cause was that the Core and Python extensions were
compiled against the host `/usr/local` NDN runtime while the SIF supplied
different same-named DSOs. Import and `ldd` checks therefore passed while the
binary ABI corrupted allocation state.

The repair is `materialize-core-bindings-fix020.sbatch`: seal the Core DSO,
`ndnsf._ndnsf`, `_py_repoclient`, and the exact NDN-SVS, NDN-CXX, NAC-ABE,
NDNSD, OpenABE, and Relic runtime libraries together. A constructor probe with
all matching DSOs reached only the expected missing-NFD error. Do not mount a
single replacement library or accept an image from import-only checks; the
whole sealed runtime must pass the constructor probe and the real three-node
gate.

### Context Mode and SVS readiness follow-up — 2026-08-01

The temporary `found 0` Context Mode failure was an authority-index gap: the
ContentDB and hooks were healthy, but the current `.specify/feature.json` and
Spec 167 authority files were missing from the file-backed source table. Use
`scripts/context_mode_index_authority.sh` after every active-Spec switch or
plan change; it re-indexes the five required sources and verifies the guard.

The subsequent fix-025/026 Qwen3-0.6B Tiger attempts failed before Selection
and inference because remote Providers had not converged on the User SVS
mapping/producer path. The previous successful Spec 160 harness waited 120 s
after Provider readiness, and historical Fix-032/033 experiments tried a
300-second variant. Those waits are retained as diagnostic negative evidence
only. The active Spec 162 source opens the User Request immediately after
process/route readiness, rejects reused fixed-settle sources, and lets ACK
coverage plus stage data dependencies drive planning and execution. These are
control-plane readiness conditions, not evidence of a model or Repo throughput
defect.

## Historical verified checkpoint (2026-07-28)

- The active replacement design is Spec 162:
  `Qwen/Qwen3.6-27B`, three distinct RTX 5000 candidate nodes, one stage per
  node, at most 64 generated tokens, five prompts, one warmup and five measured
  generations per prompt. It is planned only: current code is Qwen2-only and no
  Spec 162 live job has been submitted.
- Spec 160 proved one secured three-node Qwen2.5-0.5B layer-pipeline request on
  three RTX 5000 Ada GPUs, ranges `[0,8)`, `[8,16)`, `[16,24)`, two
  checksum-matched hidden-state transfers, exact top token `27024`, and zero
  CPU fallback.
- Slurm Job 174221 remains formally `FAILED (1:0)` because the post-run analyzer
  confused the outer identity with indexed request ID suffix `-0`. The
  immutable workload evidence is accepted only for the bounded capability
  claim; the job is not relabeled and was not rerun.
- Spec 161 capacity probe Job 174363 measured writable allocation scratch and
  passed the no-download capacity decision.
- Spec 161 32B preparation Job 174382 was submitted exactly once and was
  `PENDING (Resources)` at the recorded checkpoint. No 32B model preparation or
  distributed-generation result is claimed yet. Spec 162 neither cancels nor
  reuses that identity.

See [itiger-qwen-evidence.md](itiger-qwen-evidence.md) before accepting or
summarizing any result.

### Fix-027 control-plane result — 2026-08-01

The first retry with the restored readiness barriers was Job 181799,
submission `spec162-submission-t009-qwen3small-smoke-fix027-173631`. It used
source SHA `8063bf6b9b57bb8c4fb61684b44ecf3e254d4a57a29852ea240962c672670286`,
SIF SHA `f3da4d2147ab66e125a0695ea4fa88920f7dec7da20e7f9a0e88ce2d0a8cc7ce`,
and stage-manifest SHA
`ef9df35d362da17f61eaf5edd9dead5be8b83523ddfa408d8fd6a42fc5b694fe`.

The job reached three Provider-ready markers, Repo registration-ready, all
three selection-residency receipts, and the User request. Only Provider 0 and
Provider 1 returned ACKs. Provider 2 was started for Stage 2 but produced no
request/ACK event; the planner failed with “no feasible Provider for Stage 2”
before Selection, model fetch, GPU load, or generation. Required Stage-2
memory was 2,910 MiB. Preserve this as a Stage-2 control-plane failure, not
as a Repo throughput result, and use a new identity for the next targeted
producer/request probe.

### Fix-028 negative control — 2026-08-01

Job 181800 reused the fix-027 source/SIF/stage identities and set
`SPEC162_USER_STARTUP_SETTLE_MS=60000`. The three Provider-ready, Repo
registration, and residency barriers completed, but only Provider 0 returned
ACK. Providers 1 and 2 had no request/ACK event; the planner failed before
Selection for Stage 1. This does not implicate DistributedRepo throughput or
GPU capacity. The next run must capture per-node User certificate Interest
routing and SVS mapping delivery before another generation retry.

### Spec 162 Fix-029/030 control-plane record

Fix-029 (Job 181801) is the first small-Qwen run with all three remote
Providers visibly processing the User request and returning ACKs. It still
failed before model preparation because `offer_expiry(delta_ms=-8)` occurred
when the Selection deadline equaled the 120-second ACK window. This is a
protocol lifetime race, not a model or DistributedRepo performance result.

Fix-030 (Job 181802) keeps the 120-second ACK window and raises the offer lease
to 600 seconds, with a 30-second safety margin enforced by the launcher. Use
its terminal evidence to decide whether the next remaining boundary is Repo,
CUDA preparation, or generation. It subsequently proved that all three stage
artifacts were fetched through DistributedRepo and all three stages were loaded
on CUDA with no CPU fallback. Stage 0 published its activation, but the request
stopped because the automatic planner committed no encrypted
`scope_key_data_names` for the two inter-stage scopes. This is a DI plan-sealing
failure, not a Repo or GPU result. The captured logs are kept in the Fix-030
partial evidence directory; the Qwen generation gate remains FAILED until
Fix-031 publishes and commits those request-scoped keys before Selection.

Fix-031 (Job 181804) used the scope-key sealing bundle but remained a
pre-Selection negative control: only Provider 0 received and ACKed the first
token request; Providers 1 and 2 logged no request/ACK, and the User timed out
on Stage 1. It therefore did not exercise scope-key publication or any Repo,
CUDA, or generation path. The next run increases the Provider convergence
barrier and uses a new request identity without copying the existing model
artifacts.

### Fix-032 terminal record

Job 181805 requested a 300-second Provider convergence barrier but exposed a
launcher defect: the outer `generation-rank.sh` did not propagate
`SPEC162_PROVIDER_SETTLE_SECONDS` into the container, so the inner script used
120 seconds. Only Provider 0 ACKed the first token request; Providers 1 and 2
had no request/ACK, and the User failed before Selection for Stage 1 after
120008 ms. No scope-key, DistributedRepo, CUDA, or generation conclusion is
valid for this run. The wrapper now validates and forwards the variable with
30–900 second bounds; Fix-033 is the first valid longer-settle requalification.

### Fix-033 terminal record

Job 181806 (`spec162-submission-t009-qwen3small-smoke-fix033-190915`) used
source SHA `3ff785c7fb923073578bbc52ad0f244a6618a148fae58a3734d62c5084078f3f`.
The inner launcher visibly executed `sleep 300`, confirming that the corrected
outer wrapper propagated `SPEC162_PROVIDER_SETTLE_SECONDS=300` into Apptainer.
All readiness and Repo-registration barriers passed, but only Provider 0
received the first token request and returned an ACK. Providers 1 and 2 had no
request/ACK, and the User failed before Selection for Stage 1 after 120008 ms
(`FAIL`, exit 1). NFD recorded the expected aggregate multicast route, while
the remote faces had zero packet counters. This is a control-plane/native
request-forwarding failure; it does not measure DistributedRepo throughput,
CUDA loading, or generation and contains no scope-key or Repo-fetch evidence.
The next test must diagnose the native `beginCollaboration` publication path
before repeating the Qwen generation gate.

### MiniNDN-first gate before Tiger requalification

The existing MiniNDN LLM pipeline script was run before making any NDNSF
internal change. The first QuickChecks attempt failed only because its wrapper
omitted the script's explicit `--test-only-allow-ephemeral-app-state` switch;
the User therefore rejected the volatile journal root before a request. The
regression wrapper now supplies that test-only switch.

The corrected workload passed with MiniNDN NLSR (warmup 305.07 ms, measured
133.48 ms) and with `--static-routing-only` (warmup 346.56 ms, measured
154.43 ms). All three roles were selected, both scope-key publications were
observed, and a three-stage response was returned in both modes. Do not change
NDNSF `BeginCollaboration` or `PublishRequestV2` from the Fix-033 evidence; the
next Tiger test must compare its manual TCP group-prefix routes and live face
counters with the two passing local control paths.

### Fix-034b: User mapping lost while remote certificates converged

Job 181807 (`spec162-submission-t009-qwen3small-smoke-fix034b-194332`) used
source SHA `5c246622489349d1dbd27c622f503c389471b6b976c57ae78938ff0619737670`.
It installed the exact SVS group route and multicast strategy on all Tiger
nodes, with effective 300-second Provider and 60-second User startup settles.
All startup and Repo-registration barriers passed, but only Provider 0 received
the User request and ACKed. Providers 1/2 repeatedly validated group Data while
their User/remote Provider certificates were still absent; they never fetched
the User producer mapping. The User failed before Selection after 120008 ms;
the run contains no Repo, CUDA, or generation evidence.

MiniNDN NLSR and static-routing controls pass the identical collaboration path,
so this remains a Tiger manual TCP/SVS identity-convergence issue rather than a
generic NDNSF internal failure. Fix-035b changes only the User startup settle to
240 seconds and reuses the same content-addressed inputs.

### Current gate boundary: strict MiniNDN first — 2026-08-01

Before submitting another TigerCluster workload or treating a Docker image as
deployable, rerun the strict local MiniNDN Gate-B profile. The accepted run is
`results/spec165-minindn-first/20260801T204709Z-28c987c4`: Qwen3-0.6B ONNX,
real MiniNDN/NFD, explicit AI_Lab topology, three role selections, and complete
multi-request/multi-token lineage. The launcher rejects fake runtime
substitution and verifies each stage's artifact-ready and execution markers.

This is functional CPU evidence only. Job 181811 was canceled while this
recheck was prioritized. The full current-source Gate A-D plus container pass
is recorded below; it authorizes separately recorded external validation, not
automatic submission. Generic GPU capability probe job `181812` passed on
`itiger07`; it did not load Qwen or start NDNSF-DI.

### Current candidate-container result — 2026-08-01

The current-source aggregate
`results/spec165-minindn-first-full/20260801T205752Z-7bc46c27` passed Gates A,
B, and D but Gate C timed out. A focused rerun with the corrected
`/workspace/Experiments/Topology/AI_Lab.conf` mapping and a bounded 5-second
ACK window is
`results/spec165-minindn-first-container/20260801T211233Z-4d951ba3`.

The candidate image `ndnsf-di:spec165-minindn-gate` used real MiniNDN/NFD and
the same content-addressed Qwen3-0.6B workload, but its CPU ONNX execution
became progressively slower with three provider processes and completed only
one warmup plus three measured samples before the 630-second user wait. The
run was not OOM-killed. This is a blocking image/runtime performance result,
not evidence against NDNSF collaboration or DistributedRepo, and it keeps
TigerCluster submission paused. The next candidate must use a CPU-compatible
ONNX runtime image (or an explicitly justified runtime configuration) and
complete the unchanged workload.

### CPU candidate resolution and local closure — 2026-08-01

The CPU-compatible overlay is built from
`packaging/ndnsf-di-container/oci/Dockerfile.spec165-minindn-cpu-gate` as
`ndnsf-di:spec165-minindn-cpu-gate`; it replaces only the CUDA ONNX Runtime
wheel and exposes `CPUExecutionProvider`. Its image
digest is
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`.
The focused Gate-C run
`results/spec165-minindn-first-container-cpu/20260801T212933Z-412b76d8`
passed the unchanged two-prompt, multi-token workload. The full aggregate
`results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8` passed Gates
A, B, C, and D with `externalValidationAuthorized=true` and
`tigerClusterSubmitted=false`. This closes the local CPU/container gate; it
does not claim GPU or TigerCluster performance. The next step is a
candidate-bound standalone GPU preflight, not a larger-model campaign.

### Request-first post-ACK strategy evidence — 2026-08-03

TigerCluster job 181941 reused the existing native-overlay SIF and 0.6B
content-addressed artifacts. The sealed runtime previously lacked the
`REUSE_CACHED` member in its installed DI SDK enum; binding the current
`sdk/placement.py` into the SIF repaired that contract without rebuilding the
image. The User then executed `Request -> ACK_CLOSED -> graph/strategy ->
Repo publish or REPO_REUSE -> Selection -> Response`. The first request spent
44.9 s publishing the Repo registration; later requests reused it in 5.3–10.0
ms and Providers advertised matching GPU-resident shards.

The run also isolated a strategy-trigger latency problem: `ack_timeout_ms` was
120 s, and deferred collaboration waited for the whole window even though all
three ACKs arrived in 1–2 s. Because the Qwen harness sends one request per
generated token, this would make a 64-token smoke take about two hours. This
is not evidence that DistributedRepo is slow. The required follow-up is an
NDNSF-DI-owned validated role-coverage early-close predicate with the current
ACK window retained as a hard upper bound; it must not become pre-request
deployment or model splitting. Job 181941 was cancelled after six valid
sequential Responses and the next token's ACK collection and remains
diagnostic rather than a complete generation pass.

### 181942 complete post-ACK runtime response — 2026-08-03

The bounded diagnostic submission reused the same source/SIF/artifact identity
and set only `SPEC162_ACK_TIMEOUT_MS=10000`. It produced 47 token-level
Request/ACK_CLOSED/graph-strategy/Selection/Response cycles, one cold Repo
publication followed by 46 content-addressed reuses, and a complete EOS answer
(`exactReferenceMatch=true`). All three RTX 5000 providers executed 47 CUDA
stage steps; downstream stages recorded dependency readiness.

The raw runtime evidence is complete, but the Slurm wrapper ended
`state=FAIL`, `exitCode=1` before writing `analysis.json`; it is not a formal
campaign PASS. Preserve the partial evidence at
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-sdkbinding-10s-20260803T014744Z-001.partial`
and diagnose the wrapper/analyzer exit path next.

Architecturally this confirms the intended boundary: ACK collection closes
first, then the NDNSF-DI strategy consumes ACK cache/network metadata and the
ONNX dependency graph to resolve/reuse artifacts and commit Selection. The
application call does not pre-deploy or pre-split the model.

### 181943 atomic marker repair and recovered analysis — 2026-08-03

The follow-up reused the exact 0.6B SIF and content-addressed artifacts and
changed only the timing-marker writer. It produced 47 parseable
Request/ACK_CLOSED/graph-strategy/Selection/Response cycles, one cold Repo
publication, 46 GPU-cache reuses, and a complete EOS answer. The retained
analyzer was run independently against the copied evidence and returned
`RC=0`, `status=PASS`, with 141 CUDA stage receipts, 94 dependency receipts,
and 47 releases per Provider.

The Slurm wrapper itself still ended `state=FAIL`, `exitCode=1` before invoking
its analyzer; therefore do not report the wrapper as a formal PASS. The
recovered analysis and checksum are retained in
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-postack-atomicmarker-20260803T020650Z-001.partial`.
This closes the architectural question: the selection strategy runs only
after ACK_CLOSED, consumes ACK/cache/network plus ONNX graph inputs, and then
resolves/reuses Repo artifacts before Selection.

### 181944 official wrapper PASS and complete response

The repaired wrapper was validated by the single formal campaign
`spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001` (Slurm
`181944`, `COMPLETED`, exit code `0`). It reused the existing Qwen3-0.6B SIF,
stage manifest, Repo registration, and model artifacts. The source bundle,
SIF, and stage-manifest SHA-256 values are respectively
`d2c3f0637e6f79fc60d4c496d20e5bc7d69b21e2370afa62362ac2c162aae1fc`,
`1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`, and
`8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.

The authoritative result file is
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/spec162-submission-t009-postack-wrapperfix-20260803T022549Z-001/result.json`.
It reports `state=PASS`, a 47-token EOS response with
`exactReferenceMatch=true`, 141 stage receipts, 94 dependency receipts, zero
CPU fallback, and 47 releases per Provider. Fetch progress was monotonic with
9/15/24 events for stages 0/1/2. Cold Repo fetch was
8707.04/19418.62/27341.51 ms and cold GPU load was
5522.71/6479.60/6977.68 ms. The subsequent 46 requests hit the GPU cache on
all three Providers. This formally closes the 0.6B fetch/progress/response
gate and demonstrates the intended cold-first, warm-reuse behavior.

The wrapper's atomic timing-marker write and pre-scan removal of the ephemeral
bootstrap credential are required for a truthful PASS; they do not alter the
NDNSF-DI protocol. The production invocation remains one full-generation
Request with one ACK closure, one post-ACK strategy/Selection, and internal
prefill/decode. Output streaming, if enabled, uses ordered Response chunks
inside that invocation rather than one NDNSF collaboration per output token.
