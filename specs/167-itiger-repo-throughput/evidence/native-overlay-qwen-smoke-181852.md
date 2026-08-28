# Spec 167 native overlay and Qwen3.6 smoke evidence

Date: 2026-08-02

## Native artifact

The persistent file-stream producer change was compiled in a disposable
Ubuntu 20.04/GCC 9/Boost 1.71 builder and validated inside the runtime SIF.
No model files were included in the image.

| item | value |
|---|---|
| source | `pythonWrapper/src/ndnsf/_ndnsf.cpp` |
| source SHA-256 | `4e9350f07ef4854d4324e727412a46401b13686dfd76d0cc073b550a5d998768` |
| Dockerfile SHA-256 | `0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92` |
| extension SHA-256 | `cd233289fdc885d238876907234f23f55f3b43898dea21e9895c7da3ab69d2fb` |
| SIF SHA-256 | `1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368` |
| remote release | `/project/tma1/ndnsf-di/releases/spec167-native-file-producer-0f834dbaad7496628fb31bfcdfc87f6ead0874a03db65329c5536d8b1da63d92/runtime.sif` |

## Smoke 181852

Submission `spec162-submission-t009-qwen36-native-repo-20260802T141500Z-001`
reused the existing Qwen3.6-27B BF16 stage artifacts and stage manifest. All
integrity, repository-registration, provider-preparation, and CUDA-residency
barriers passed. The three stages occupied approximately 18.2 GiB, 15.6 GiB,
and 18.7 GiB of GPU memory after loading.

The execution did not pass. Stage 1 timed out after 30 seconds waiting for
`tensor-0-dfb7e82149ba03ac`; Stage 2 timed out after 30 seconds waiting for
`tensor-1-aa6de4efc3538d07`. Stage 0 eventually published its 380,824-byte
activation successfully, but too late for the downstream fixed wait. Slurm
job 181852 was cancelled at 00:48:40 after preserving the partial result:

`/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-native-repo-20260802T141500Z-001.partial`

This is a reproducible dependency-deadline failure. It must be fixed before
claiming end-to-end distributed generation or submitting the formal campaign.

## Follow-up source repair and requalification

The fixed wait was removed from the active Qwen path. `ProviderRuntimeContext`
now receives the signed Selection assignment deadline and computes remaining
dependency time for reference wait, Repo fetch, and result Future wait. The
deadline is fail-closed after expiry, while legacy non-V2 contexts retain a
bounded fallback. The ONNX path uses the same helper. Focused provider and
contract tests pass, and the current-source real MiniNDN Gate B recheck passed
at:

`results/spec165-minindn-deadline-recheck-user-2/20260802T151603Z-c92c19b6`

The Tiger smoke reuses this existing SIF and stage manifest with a new source
bundle (no model or foundation rebuild):

* Slurm job: `181853`;
* source manifest: `3a31ffe973ecf850a5cbc218124579603d7847ec9888bacc737b66c727930c08`;
* SIF: `1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`;
* stage manifest: `cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`.

The job has passed source, SIF, stage, GPU, NFD, and cross-node route barriers;
final inference status was not produced; this checkpoint is not a successful
generation result.

## Follow-up job 181854: exact route did not close the Repo failure

Submission `spec162-submission-t009-qwen36-route-20260802T1052Z-001` (Slurm `181854`) reused the exact SIF, Qwen3.6-27B stage files, and stage manifest. It passed source/SIF/stage checks, NFD and route barriers, Repo registration, all three successful ACKs, all Selection handlers, and all collaboration handlers.

The data-plane result was negative: before intentional cancellation at `00:58:54`, Stage 0 fetched about 18.00/18.53 GB, Stage 1 about 0.26/15.99 GB, and Stage 2 about 0.042/19.27 GB. Stage 2 failed with `ArtifactApiError INTERNAL_ERROR: artifact backend failed`; no stage reached GPU residency and no response was produced. The retained result is `/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/.spec162-submission-t009-qwen36-route-20260802T1052Z-001.partial/result.json` (`state=CANCELLED`, `exitCode=130`).

This isolates the remaining blocker to concurrent Repo segmented retrieval or timeout/error handling, rather than the prior dependency deadline or the NDN producer-name route. The public facade remains bounded by default; the next trusted diagnostic run may set `NDNSF_ARTIFACT_DEBUG_ERRORS=1` to retain the native local exception. No formal warmup-plus-five campaign was submitted.
## Small-model request-ID requalification — Slurm 181859

Submission `spec162-submission-t009-qwen3small-requestid-20260802T171609Z-001`
reused the same SIF and the existing Qwen3-0.6B content-addressed stage
manifest. The source bundle was
`10617d5d5cef7a84efb8da7f2c8a607bb26a7af6fc15e38a2ccd56f8187c2266`; the
retained result is
`/project/tma1/ndnsf-di/evidence/spec162/qwen3-0.6b-smoke/.spec162-submission-t009-qwen3small-requestid-20260802T171609Z-001.partial/result.json`.

All control-plane barriers and the distributed response path progressed. Two
token responses were received after the User-side canonicalization accepted
the NDN wire spelling `/request-id` as the same invocation as the application
spelling `request-id`. Their measured token-step totals were 170793.84 ms and
120129.62 ms. No request-ID mismatch occurred. The smoke was intentionally
cancelled while token 2 was running (`state=CANCELLED`, `exitCode=130`) because
the harness waits 120 seconds for each token's ACK window; this is diagnostic
evidence, not a formal generation or throughput result. No model, SIF, or
foundation rebuild was performed, and no formal warmup-plus-five campaign was
submitted.
