# Spec 170 Gate B MiniNDN Evidence (current checkpoint)

**Run directory**: `results/spec170-gate-b-minindn-20260805T103000Z/`  
**Date**: 2026-08-05  
**Verdict**: **BLOCKED; not a freeze candidate**

## Campaign completeness

The campaign created five block directories and the expected manifests/logs,
but it did not produce the required three clean-start cold/warm blocks for
`P01-P05`:

| Block | `generation.jsonl` rows | Result |
|---|---:|---|
| block-1 | 6 | a few successful rows, then interruption/timeout |
| block-2 | 0 | no completed generation rows |
| block-3 | 0 | no completed generation rows |
| block-4 | 0 | no completed generation rows |
| block-5 | 0 | no completed generation rows; timeout/authentication failures |

The required measured sequence is therefore incomplete. The six `OK` rows in
block-1 are retained as diagnostics and are not a cold/warm statistical block.

## Failure evidence

Provider logs contain repeated `Collaboration data authentication failed`
messages. User/provider logs also contain timeout/interruption evidence. Block-5
shows a large uncached Repo transfer followed by selection/preparation and then
authentication failures; this is not a valid exact-warm reuse observation.

These failures occur before a complete three-Provider, multi-token workload can
be claimed. They must be isolated with the in-process L1/L2 harness before
restarting the full campaign; do not paper over them by changing the measured
profile or replacing failed rows with readiness-gated steady-state results.

## Post-fix MiniNDN protocol smoke

The failure was localized to provider-scoped assignment metadata. Deferred
selection envelopes now filter `scopeKeyData.*` references by the selected
role's producer/consumer scopes, and the Provider treats the compact shared
metadata as a legacy fallback only for unstructured assignments. The native
unit regression covers the envelope projection; a root-run MiniNDN smoke then
completed a real three-Provider fake pipeline with one measured request:

```text
output: results/spec170-scope-filter-minindn-smoke-20260805T071600Z/
LLM_PIPELINE_MININDN_OK local_ms=11.59 distributed_ms=381.24 stages=3 runtime=fake
```

The User log recorded provider-specific assignment projections of 611, 809,
and 611 bytes for Stage 0/1/2. The three Provider logs contain zero
`Collaboration data authentication failed` lines, and the User log contains one
successful measured response. This is evidence that the wrong-scope-key
authentication failure is repaired on the MiniNDN control path, but it is not a
Qwen real-model or cold/warm statistical result.

Smoke log hashes:

```text
564a4e45f49d85a593618ea1f936d1accb226bd95471964169691e8b0b9e655f  results/spec170-scope-filter-minindn-smoke-20260805T071600Z/llm-pipeline-user.log
fe7050392db1a5d05f9e3091e4d52273d74ff98dd67b3941a52e0b9682c1d1f2  results/spec170-scope-filter-minindn-smoke-20260805T071600Z/stage0-provider.log
8582257939830530a5d02ce0b8360d25e25a65459d48d452741a714e6eb4a1c9  results/spec170-scope-filter-minindn-smoke-20260805T071600Z/stage1-provider.log
96c5c1969a0b2a48b5a5a0f784b58e4ea12ca5bb2708dc323f151bbeb429da02  results/spec170-scope-filter-minindn-smoke-20260805T071600Z/stage2-provider.log
```

## Qwen preflight attempt after the fix

A single request-first Qwen3-0.6B MiniNDN attempt reused the immutable local
stage bundle and reached the real provider startup preflight, but Stage 0
failed before opening its service because the current node image provides
`transformers==4.46.3` without `transformers.models.qwen3`:

```text
RuntimeError: QWEN_TRANSFORMERS_MODEL_TYPE_UNAVAILABLE:qwen3:ModuleNotFoundError:No module named 'transformers.models.qwen3'
```

Run directory: `results/spec170-scope-filter-qwen-minindn-20260805T072320Z/`.
This is an environment/preflight blocker, not a collaboration-authentication
result; no Qwen response row was produced. The Stage 0 log hash is:

```text
a6ed629fdd9ca1264424f377fb7f2b937954a74b3e2a93abdee43a574668cdba  results/spec170-scope-filter-qwen-minindn-20260805T072320Z/stage0-provider.log
```

## Follow-up control-path diagnostic

After rebuilding the candidate with the environment-gated
`NDNSF_COLLAB_AUTH_TRACE` diagnostic, a one-request, three-Provider MiniNDN
`runtime=fake` run completed successfully:

```text
LLM_PIPELINE_MININDN_OK local_ms=20.27 distributed_ms=281.69 stages=3 runtime=fake
```

The producer and consumer trace for both collaboration edges reported identical
scope-key and associated-data digests. This confirms that the generic
publish/decrypt envelope is internally consistent for the short control-path
case, but it is not a Qwen workload result and does not replace the failed
real-model rows above. The trace was captured in
`/tmp/ndnsf-spec170-auth-trace-IadkgT` and is retained only as a diagnostic
checkpoint.

## Gate decision

**FAIL/BLOCK**: Gate B does not satisfy T026. T029 freeze and all TigerCluster D
gates remain prohibited. The next executable step is a deployment-faithful
Qwen MiniNDN cold/warm rerun using the repaired candidate, followed by the
remaining L1/L2 protected-data and three-Provider selection cases. Only a clean
small-model Gate B campaign can authorize the user-approved TigerCluster run.

## Deployment-faithful Qwen runtime diagnostic (2026-08-05)

The host's Transformers 4.46.3 preflight blocker was removed for this diagnostic
only by extracting the exact local OCI runtime (Python 3.10, Torch 2.6.0+cu124,
Transformers 5.14.1) into `/tmp/spec170-qwen-runtime-v514-20260805`; no source
or MiniNDN harness change was made. The immutable three-stage Qwen3-0.6B bundle
and tokenizer artifact were unchanged. The existing harness was run with the
deployment-faithful `qwen-transformers` path, V3 request-first selection, three
real Providers, real DistributedRepo, and a two-token frozen campaign.

The first run (`results/spec170-qwen-runtime-v514-two-token-20260805T/`) used a
180-second request deadline and stopped after stage 0 and stage 1 Repo
registration; stage 2 was still being published when the request failed. A
retry with the same workload and a 360-second request deadline was run at
`results/spec170-qwen-runtime-v514-two-token-retry-20260805T/`. It completed
the deployment and selection boundary:

```text
ACK_CLOSED placement=V3 ackCount=3
QWEN_REPO_STAGE_REGISTERED stage=0 bytes=594357850
QWEN_REPO_STAGE_REGISTERED stage=1 bytes=283192711
QWEN_REPO_STAGE_REGISTERED stage=2 bytes=625825930
QWEN_DYNAMIC_CATALOG_ACTIVE
NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED selectedCount=3
```

All three Providers reached `LLM_PIPELINE_QWEN_RUNTIME_READY` and entered the
full-stage handler, but the measured request produced zero tokens and failed
after 360,022.997 ms. Stage 0 and stage 1 timed out waiting for the next token
while stage 2 was still finishing its cold Repo fetch; the logs also recorded
three collaboration authentication failures for data delivered outside the
receiving role's key scope. No complete multi-token response, warm hit, or
valid cold/warm block was produced. This is stronger deployment evidence than
the earlier Qwen import preflight, but it remains a T026 failure and does not
authorize TigerCluster.

Selected immutable evidence hashes:

```text
d12a5709937d827a7fb028fb76711315f3ee06dbfe1f27ce7e40b7ddbe8b7d26  results/spec170-qwen-runtime-v514-two-token-retry-20260805T/llm-pipeline-user.log
3475c5454c2d94d85a5673c47d26b0e0ee2592724e2e9284b188b0d0e0eae340  results/spec170-qwen-runtime-v514-two-token-retry-20260805T/stage0-provider.log
88016e677a2b8c6868e4b58dbccbd7ce0c20f585573da1b49da4f1626d7208eb  results/spec170-qwen-runtime-v514-two-token-retry-20260805T/stage1-provider.log
b6cfb5f6d0b4b42a45568b881094008fe0c397db431ead340373cf66cb2925c2  results/spec170-qwen-runtime-v514-two-token-retry-20260805T/stage2-provider.log
71c953410bffcd365cbf3dffc793bbaa365d4222b08bd2be7eaf579b891cfba0  results/spec170-qwen-runtime-v514-two-token-retry-20260805T/generation.jsonl
```

## Deployment-faithful Qwen deadline-propagation diagnostic (2026-08-05)

The V3 projection previously carried no execution deadline, so the Provider
fell back to its 30-second dependency timeout even when the request declared a
360-second cold-start budget. The fix is deliberately narrow: carry and validate
`deadline_ms` in `ProviderSelectionProjectionV3`, preserve it when the Python
placement coordinator seals the projection, and decode it in the Provider's
assignment deadline helper. The Spec 170 Python suite is green after the fix:

```text
58 passed, 2 skipped, 1 warning in 7.43s
```

The deployment-faithful rerun is retained at
`results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/`. It used
the immutable three-stage Qwen3-0.6B bundle, the extracted local OCI runtime
(`torch==2.6.0+cu124`, `transformers==5.14.1`), real MiniNDN/DistributedRepo,
three CPU Providers, V3 request-first selection, and the frozen two-token
campaign. It completed the previously failing cold path:

```text
QWEN_REPO_FETCH_COMPLETE stage=0 bytes=594357850
QWEN_REPO_FETCH_COMPLETE stage=1 bytes=283192711
QWEN_REPO_FETCH_COMPLETE stage=2 bytes=625825930
QWEN_RUNTIME_READY stage=0,1,2
NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED placement=V3 selectedCount=3
QWEN_FULL_GENERATION_FINAL tokenCount=2 stopReason=TOKEN_LIMIT
GENERATION_FINAL_RESPONSE status=OK tokenCount=2 responseBytes=3
GENERATION_CAMPAIGN_PASS promptCount=1 warmupSamples=0 measuredSamples=1
```

`generation.jsonl` reports generated token IDs `[8065, 45]`, decoded text
`NDN`, and `exactReferenceMatch=true`; the measured request took
`341537.776` ms with `ttftMs=338317.376`. This proves the deadline fix and a
complete real-model two-token response, but it is not the required T026 matrix:
there is no warmup/five-request repetition and only one cold-start block.

The strict diagnostic gate also remains blocked because the three Provider logs
contain ten `Collaboration data authentication failed` entries for broad
out-of-role deliveries (five in Stage 0 and five in Stage 2). The valid
stage-to-stage chain still completed, but a clean T026 campaign requires these
unexpected authentication errors to be eliminated or explicitly filtered and
then rerun under the unchanged workload.

Immutable hashes for this run are:

```text
20955762fe0e6c8fb331b7e911597817a16e309922ca438099321ae079d30314  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/llm-pipeline-user.log
9f59af71cb871990067af066aabdc2af9f373caef1474a354d77d272e3072c69  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/stage0-provider.log
91fa7e572310793264cf273e8a9caa61dfd34bf1bf76db33ed83aea55c6b83d6  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/stage1-provider.log
08aaaddc01aaed17594b5ab80ad3a2a803a986e757f2a44f486744b83c4a9bea  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/stage2-provider.log
4a27b1929a4c0e0bcffce202efa179ac85303193e159029e6f4da40894e15a3d  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/generation.jsonl
df3e884159b6dd7682a2c32ae69aba269eb23aa2b63aa92fd21274d26bd28efb  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/automatic-planning.json
37971e78ba700f8f2c5ebb268bedad800307e8a3af6562ed32f54f103b405b3c  results/spec170-qwen-runtime-v514-two-token-deadline-fix-20260805T/stage-manifest.runtime.json
```

This remains diagnostic evidence, not a Gate B PASS; TigerCluster D gates are
still prohibited until a clean small-model campaign satisfies T026.

## Pre-decrypt role-scope filter diagnostic (2026-08-05)

The ten out-of-role authentication failures were fixed by adding a
request-scoped native `CollaborationContext::allowData(keyScope, topicPrefix)`
receive filter. V3 Python Providers register every local input/output/internal
dependency immediately after decoding the sealed projection, before artifact
preparation. The broad SVS subscription is retained, but a packet whose
scope/topic does not belong to the receiving role is discarded before
scope-key lookup or AEAD authentication. Existing `subscribe()` and unfiltered
legacy requests keep their previous behavior.

A root-run MiniNDN control-path diagnostic with the rebuilt Python 3.8 binding,
three real Providers, one measured fake request, and
`NDNSF_COLLAB_AUTH_TRACE=1` completed successfully:

```text
output: results/spec170-scope-filter-minindn-postfix5-20260805T/
NDNSF_DI_CLIENT_INFERENCE_TIMING service=/AI/LLM/Pipeline/Fake mode=deployed scope_key_ms=10.60 request_ms=377.53 total_ms=388.14 status=true
LLM_PIPELINE_USER_RESPONSE ... stages=3 runtime=fake ...
LLM_PIPELINE_USER_SUMMARY count=1 ...
```

There are zero `Collaboration data authentication failed` lines across the
three Provider logs. The trace contains only the two valid consumer decrypts
(Stage 1 consumes Stage 0; Stage 2 consumes Stage 1); unrelated packets are no
longer attempted. This validates the filter on a real MiniNDN carrier, but it
is still a fake-runtime diagnostic and does not satisfy the T026 cold/warm
Qwen matrix.

Immutable diagnostic hashes:

```text
c7552c571d83d130ffedfc524d2de06f6ff6b05bc6623de56ccafad0f1fc0219  results/spec170-scope-filter-minindn-postfix5-20260805T/llm-pipeline-user.log
9d78ba28bfa73ea939f9e6aff6ff37249a5ae4f4b8648605143e7501d6e2f005  results/spec170-scope-filter-minindn-postfix5-20260805T/stage0-provider.log
e7c1f2336f1e9e4706743c89e0461e84a78dd4da7260124fa6620ebe0927bc74  results/spec170-scope-filter-minindn-postfix5-20260805T/stage1-provider.log
6dbf23719ae400820c80c743f89c518f91b216c0715ff32c09593048c362ab79  results/spec170-scope-filter-minindn-postfix5-20260805T/stage2-provider.log
97400ab4946fcdcc8b54a158b52e45000a384411eb77dcee8ceb63f0942f2ee2  results/spec170-scope-filter-minindn-postfix5-20260805T/llm-pipeline-user-measured.csv
```

## Post-filter deployment-faithful Qwen diagnostic (2026-08-05)

A follow-up real Qwen3-0.6B MiniNDN run used the same immutable stage bundle,
Python 3.10/Transformers 5.14.1 runtime, V3 request-first selection, and a
two-token campaign. Stage 0 and Stage 1 fetched their assigned fragments and
reached `QWEN_RUNTIME_READY`; Stage 2 completed its 625,825,930-byte fetch but
did not finish CPU runtime readiness before the unchanged 360-second request
deadline. The request therefore failed with `FULL Qwen token result timed out`
and produced no response row. This is a cold-start/runtime-capacity failure,
not an authentication failure: all three Provider logs contain zero
`Collaboration data authentication failed` entries after the pre-decrypt filter.

Run directory: `results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/`.
It confirms the filter does not reintroduce the earlier ten auth failures, but
it still does not satisfy T026's complete response or cold/warm repetition
requirements.

```text
76b7885bf49e5e97bb4ad910226351ec369b64f46f35c25ed930371f92042059  results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/llm-pipeline-user.log
7837ba1cb724152555fcb529844ceae5133cf7b17fa7569a8651dcec9265090e  results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/stage0-provider.log
76bb87f6da5fa05cd77dcf994fff409da7b295f947ec4681a5e78527173c2c91  results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/stage1-provider.log
8449b23da5fdbed0852a4f0f4fc6c17e9a8f7b6501b050fecbb334b7b08152f1  results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/stage2-provider.log
0d0c3278a10cb2dfe58a0883585d86a39841587c79bcf4e05dbf1004ac746db0  results/spec170-qwen-runtime-v514-two-token-scope-filter2-20260805T/generation.jsonl
```

## Low-memory Qwen loader diagnostic (2026-08-05)

The cold-start capacity issue was narrowed to the concurrent stage loader's
peak memory, rather than a protocol or collaboration-authentication failure.
The Qwen Transformers loader now uses `torch.load(..., mmap=True)` when the
runtime supports it and constructs the stage module on `meta` before attaching
the loaded tensors with `assign=True`; older PyTorch versions retain the
compatibility path. This change is in the runtime source only and does not
alter the MiniNDN harness, model bytes, routes, or security policy.

The direct loader benchmark on the extracted immutable runtime
(`torch==2.6.0+cu124`, `transformers==5.14.1`) measured these resident-set
peaks, with one stage loaded per process:

```text
stage=0 load_s=6.151 warm_s=0.046 rss_kb=1995400
stage=1 load_s=5.132 warm_s=0.041 rss_kb=1083216
stage=2 load_s=5.523 warm_s=0.070 rss_kb=1750672
```

The deployment-faithful retry used the same real MiniNDN three-Provider V3
path, immutable stage bundle, and two-token reference campaign. All stages
fetched their assigned fragments, reached `QWEN_RUNTIME_READY`, and produced
the exact token IDs `[8065, 45]` (`NDN`) with zero collaboration-authentication
failures:

```text
run: results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/
QWEN_RUNTIME_READY: stage=0,1,2
GENERATION_FINAL_RESPONSE: status=OK tokenCount=2 exactReferenceMatch=true
GENERATION_CAMPAIGN_PASS: promptCount=1 warmupSamples=0 measuredSamples=1
auth failures: stage0=0 stage1=0 stage2=0
```

This is a successful deployment diagnostic, not T026 closure: it has one cold
request, no warmup, and one measured sample rather than the locked `P01-P05`
three-block sequence. T026 and Gate B therefore remain `BLOCKED`, and no
TigerCluster job is authorized yet.

Immutable diagnostic hashes:

```text
fa16321a4fe4d1df3a802b229a2561555de89bd2c4936158d47f97b129dd7506  results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/llm-pipeline-user.log
1556ffffc04e3dca1f90146d016d73cfba99fcd01538cd1bff324f320d85a397  results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/stage0-provider.log
33f9b1ac3d958bb054d8cdf463400c2d264cdb710215e027dbba27217626ecc6  results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/stage1-provider.log
a37d6a348e336e873de3820be66ea4d3698ee50d4f43736fd5ebb4f140713c4d  results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/stage2-provider.log
8d8a5d5e63b5f5f263f752d8b6343390e79668f27dcf069c4952ff84aff3b0e6  results/spec170-qwen-runtime-v514-lowmem-filter6-20260805T/generation.jsonl
```

## Extended-timeout campaign diagnostic (2026-08-05)

The first detached retry used the locked five-prompt campaign but retained
`--timeout-ms 900000`.  The existing MiniNDN runner derives its outer User
wait from that request deadline, so it stopped at 930 seconds before the
campaign could finish.  Four rows completed successfully (`P01` warmup and
`P01` measured repetitions 0-2); no response/authentication failure was
observed in those rows.  The run is a retained timeout diagnostic, not a
partial statistical block and not Gate B evidence.

Run directory: `results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/`.
The terminal status is `exit.rc=1`; the launcher log records
`subprocess.TimeoutExpired ... user process timed out after 930.0 seconds` and
MiniNDN cleanup completed.  The subsequent formal retry uses the same
unchanged campaign and runner with only the request deadline extended to
`7200000` ms, so the 30 locked rows can finish without modifying the harness.

Retained hashes:

```text
1b4021f306f15688a3321169aa23d47001fa5ede09c32b519db62bd1ffb6b33c  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/generation.jsonl
65bd7d3e513f7cf774a69a66a9f35aa3cc170ef1c009e5a250067faf4f283284  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/llm-pipeline-user.log
add52a2e39ac9b4750e7a43feef88795b8788d0dbd4af05da6fe468f8fd34249  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/stage0-provider.log
aba20e3d5ad8a01c254dc504575cb923091253f523e3c9f7a70130aad41cf69e  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/stage1-provider.log
9741ec63c6bbfaada70005fe486487c0283dd745e632637612112c1f6b92beb9  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/stage2-provider.log
4355a46b19d348dc2f57c046f8ef63d4538ebb936000f3c9ee954a27460dd865  results/spec170-qwen-runtime-v514-t026-block1-rerun-20260805T/exit.rc
```

## Formal T026 block-1 result (2026-08-05)

The unchanged deployment-faithful MiniNDN runner completed one formal locked
`P01-P05` block with the extracted immutable Qwen Transformers runtime
(`torch==2.6.0+cu124`, `transformers==5.14.1`), three real CPU Providers,
V3 request-first selection, and request deadline `7200000` ms. The campaign
produced exactly 30 rows: for each prompt one unmeasured warmup and five
measured requests. Every row was `status=OK`, had the expected complete
multi-token output, and had `exactReferenceMatch=true`; P01 produced 41 tokens
and P02-P05 each produced 64 tokens. The campaign marker reported
`promptCount=5 warmupSamples=5 measuredSamples=25`, and MiniNDN cleanup
completed. The launcher did not emit a separate `exit.rc`, so the marker,
process termination, cleanup tail, and row-level verifier are retained as the
terminal evidence instead of inferring an exit code.

The same run also recorded 30 V3 selection commits, 90 `QWEN_RUNTIME_READY`
events, 90 residency events, 90 assigned-fragment Repo fetch completions, and
zero `Collaboration data authentication failed` lines in each Provider log.
This is **T026 block-1 PASS**, not Gate B closure: T026 still requires two
additional clean-start blocks, the exact normal-default V3 concurrency claim,
and the predeclared statistical/failure intervals. Gate B, T028, T029, and
TigerCluster therefore remain blocked.

Run directory: `results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/`.

Immutable hashes:

```text
0f86bcc217bcf767257e5e498ccd621d2c92c890b21a288ade854dd1d42b0ca9  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/generation.jsonl
e3d2eeee00d19eb3739b46fa9bf7e1e72bf42925c0b5d7974459f8f9be740c77  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/llm-pipeline-user.log
fd51af2a67c8e7809ebd1bef3ce531796a0bb91d9993a33a22eeca2ab4538d56  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/stage0-provider.log
f892bdcafcd137e5746efb58b0f7057225fbd583d1820fa3daa22f491803c5df  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/stage1-provider.log
537e88fc05ea4a653927ea7c72b526affd3e4be17b89acb1469df3ffafc5b21f  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/stage2-provider.log
1106dc547e51cfb4e47844abff264f057cbda7d22d212d4ca19e78fbb772353c  results/spec170-qwen-runtime-v514-t026-block1-formal-20260805T/launcher.log
```

## Formal T026 block-2 result (2026-08-05)

The unchanged deployment-faithful MiniNDN runner completed a second clean-start
locked `P01-P05` block with the same immutable Qwen Transformers runtime,
three real CPU Providers, V3 request-first selection, and request deadline
`7200000` ms. The campaign produced exactly 30 rows: one unmeasured warmup and
five measured requests for each prompt. Every row was `status=OK` and
`exactReferenceMatch=true`; P01 produced 41 tokens with `EOS`, while P02-P05
produced 64 tokens with `MAX_NEW_TOKENS`. The campaign marker reported
`promptCount=5 warmupSamples=5 measuredSamples=25`, and the MiniNDN launcher
recorded `*** Done` during cleanup. No separate `exit.rc` was emitted, so the
marker, cleanup tail, process termination, and row-level verifier are retained
instead of inferring an exit code.

The block recorded 30 V3 selection commits and 30
`QWEN_RUNTIME_READY`/model-residency/assigned-fragment Repo-fetch-complete
events in each Provider log. The first request fetched each fragment over the
carrier; subsequent fetch-complete events had `lastSegment=-1`,
`deliveredSegments=0`, and `fetchMs=0.00`, while the user log recorded 29
`LLM_PIPELINE_QWEN_DEFERRED_REPO_REUSE` events. This is the expected cold then
warm local-fragment reuse pattern. All three Provider logs contain zero
`Collaboration data authentication failed` lines.

The block therefore passes its own locked cold/warm row contract, but it is not
Gate B closure: T026 still requires a third clean-start block, the exact
three-concurrent normal-default V3 invocation evidence, and the predeclared
statistical/failure intervals. Gate B, T028, T029, and TigerCluster remain
blocked.

Run directory: `results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/`.

Immutable row verifier output:

```text
rows 30
P01 rows=6 warmup=1 measured=5 statuses=['OK'] tokens=[41] exact=[True]
P02 rows=6 warmup=1 measured=5 statuses=['OK'] tokens=[64] exact=[True]
P03 rows=6 warmup=1 measured=5 statuses=['OK'] tokens=[64] exact=[True]
P04 rows=6 warmup=1 measured=5 statuses=['OK'] tokens=[64] exact=[True]
P05 rows=6 warmup=1 measured=5 statuses=['OK'] tokens=[64] exact=[True]
all_ok True
all_exact True
has_campaign_marker True
```

Immutable hashes:

```text
ea87e97cfd0675569383aba854f5b23d1b2967d5974f24f067da3dbcbf08d9c8  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/generation.jsonl
ae7cf131fe85cb23ca620c3a4c37f4040a0427bc96aee0e2963c365f35a6e921  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/llm-pipeline-user.log
d997bc8b3efe72736e57220184ac8d841a2b7d41ed54129b2b8cf4f0248c72f1  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/stage0-provider.log
8300116b4238b03c87d4f63da8fdf2e6c635939ec1ebbf11c287105f1d79442b  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/stage1-provider.log
dc785859c2b03c3f75756b14ab98f39cec6cf15a92281deb1a505a90bd87687c  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/stage2-provider.log
994cd0d6c0d0024778c2e665d836dbc0a5bb997182f9605bffd09e6a88a1ae81  results/spec170-qwen-runtime-v514-t026-block2-formal-20260805T/launcher.log
```

## Formal T026 block-3 run stopped by scope decision (2026-08-05)

The third deployment-faithful MiniNDN Qwen run was intentionally stopped after
the local CPU execution had already consumed substantial wall time.  This is a
user-directed scope decision because the host has no GPU; it is not a runtime
failure and it is not a completion claim for T026.  The retained directory is
`results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/`.

At interruption, the run had written 17 successful rows (P01-P03, including
the active P03 measured sequence), and the three Provider logs each contained
17 `QWEN_RUNTIME_READY`, model-residency, and assigned-fragment Repo-fetch
completion events with zero collaboration-authentication failures.  The last
request was interrupted while processing P03 measured repetition 4; its user
log ends with `KeyboardInterrupt`.  MiniNDN processes were then terminated by
their exact launcher process IDs and no matching run processes remain.

The partial rows are preserved as diagnostic flow evidence only.  They do not
replace the locked T026 requirements (three clean-start blocks, the exact
normal-default V3 concurrency claim, and the predeclared statistical/failure
intervals).  Therefore Gate B, T026, T028, T029, and TigerCluster remain
blocked; subsequent local validation uses the short fixture/integration flow
tests instead of another multi-hour CPU generation campaign.

Retained interrupted-run hashes:

```text
3fb35a512248046cca4c0d2a95773b4ee9566055255a8628be11115ccd44d986  results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/launcher.log
524adc8fa1cb24b754d220ee35a18851cff421aee6bc5e7e408fe91daa913e81  results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/llm-pipeline-user.log
a9f4f5b71abe44e7ab7bfb587a471f0a2833b56de10dc2602cb26ec3cf71fd1a  results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/stage0-provider.log
0fb70d525f7bed7584f5a95727a3da9fcf5069da20eab6de5992591d1736ab17028a  results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/stage1-provider.log
2ee6db6a030c4659f8addba7a5c59dab8a584f54a040a22940a3cfab032cda3b  results/spec170-qwen-runtime-v514-t026-block3-formal-20260805T/stage2-provider.log
```
