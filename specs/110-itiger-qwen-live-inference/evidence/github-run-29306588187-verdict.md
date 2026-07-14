# GitHub GPU assembly run 29306588187 verdict

**Verdict**: `EXECUTED_FAIL`; this candidate identity is frozen and MUST NOT be
rerun. T170's exactly-once dispatch passed, but T171 produced no final runtime
digest. This is not CUDA, SIF, Slurm, or Qwen evidence.

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
| Terminal state | `completed/failure`, `2026-07-14T05:02:35Z` |
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
No automatic rerun occurred or is permitted.

## Measured terminal failure

The replacement passed the Foundation runtime-package ordering boundary that
failed run `29297628957`. It then compiled and linked all 244 NDNSF/GPU-native
workspace actions and installed the three local Python packages. The final
assembler closure scan failed on an optional torchaudio/torio FFmpeg plugin:

```text
RuntimeError: RUNTIME_LIBRARY_MISSING:/opt/venv/lib/python3.10/site-packages/torio/lib/_torio_ffmpeg6.so
```

`derive-runtime-packages.py` scans every shared object below `/opt/venv`, even
when a shared object belongs to an optional backend that the Qwen text runtime
does not load. The CUDA PyTorch wheel set explicitly installs `torchaudio`,
which carries several version-specific optional FFmpeg plugins. The scan
therefore treated the unused FFmpeg 6 plugin as a mandatory runtime surface and
failed closed before the native ONNX link check and Python import gate.

The runner had 89 GB free before the build and 62 GB free afterward, so disk is
again excluded. The final runtime tag is absent. No release manifest, SBOM,
signature, SIF, Slurm allocation, CUDA-provider observation, or Qwen result was
produced.

## Preserved evidence

The ignored evidence directory is
`results/spec110-itiger-qwen-live/github-dispatch-t170/`.

| Artifact | Digest |
|---|---|
| Text run log, 691546 bytes | `sha256:63185c40a738c51ad428d91dfa14b29f99e26af5bad95ddab2025f0d5f23369c` |
| Complete log archive, 176041 bytes | `sha256:be96d0d2b1f98aeadf13be57d62eab759360fc5bba61ff73a78b2d02b550f265` |
| Buildx record, 132214 bytes | `sha256:63a34765283fef025c438f082178b9c4bc226d2777a41c7e0bc5fc24799253bd` |
| Release-evidence archive, 1831 bytes | `sha256:63edb739e46d7a8f3dbda89f4180968c8e55b2a4f58ebceab9ddd2f405e14864` |

The preflight recorded eight locked sources, 40 Python packages, and 11 CUDA
system requirements. The source scan covered 79 files and 249085 bytes with
zero secret findings. These passing preliminary checks do not override the
terminal build failure.

## Replacement boundary

Run 8, its source tag, Foundation digest, and release identity are immutable
negative evidence. A further repair must use a new committed source identity,
new source-bound Foundation tag/digest, new release identity, explicit human
authorization, and one new dispatch. It must first prove which Python native
extensions are required by the Qwen runtime and must not silence unresolved
libraries in required extensions. T160 remains locked because T171 did not
PASS.
