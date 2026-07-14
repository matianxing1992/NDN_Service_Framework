# GitHub GPU assembly run 29306588187 verdict

**Current verdict**: `IN_PROGRESS`. T170's exactly-once dispatch passed; T171
must preserve the terminal digest or exact failure before this document can
make a release verdict. This is not yet CUDA, SIF, Slurm, or Qwen evidence.

## Immutable identity

| Field | Value |
|---|---|
| Workflow run | `29306588187` (run 8, attempt 1) |
| URL | `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/29306588187` |
| Event | `workflow_dispatch` |
| Source revision | `8f6332ce800a1a5130f457fa54454ad968dff638` |
| Source ref | `spec110-gpu-source-8f6332ce800a1a5130f457fa54454ad968dff638` |
| Release identity | `spec110-runtime-8f6332ce800a1a5130f457fa54454ad968dff638` |
| Foundation input | `ghcr.io/matianxing1992/ndnsf-di-foundation@sha256:a9ed75a9fa09acd6e795007e5d58a69fb9f9b349222faab9aeb852c70fbed820` |
| Dispatch accepted | `2026-07-14T04:41:25Z`, HTTP 204 |
| Retry policy | `NO_AUTOMATIC_RERUN` |

## T170 dispatch gate

Before creating the source tag, the gate proved that the workflow was active,
the source and release identities differed from frozen run `29297628957`, the
critical GPU workflow/Dockerfile/lock/preflight files were byte-identical to
the reviewed source commit, the new Foundation digest was anonymously
readable, the final runtime tag did not exist, and no workflow run existed for
the new source ref. The annotated source tag resolves exactly to the reviewed
commit.

A crash-safe `INTENT_DURABLE` record was fsynced before calling the dispatch API.
The API was called once, returned HTTP 204, and reconciliation found exactly
one run with the expected source ref and SHA. The ignored operational journal
is retained at
`results/spec110-itiger-qwen-live/github-dispatch-t170/dispatch-record.json`.
No automatic rerun is permitted. T171 now owns terminal monitoring.
