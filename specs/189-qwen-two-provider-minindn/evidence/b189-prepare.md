# B189-1 Preparation Evidence

**Status**: IN_PROGRESS / NOT_NATIVE_PASS

The real local Qwen3-0.6B snapshot was loaded and a two-stage artifact export was completed in
`.codex-tmp/spec189-qwen-two-provider-20260918/`. The current external canonical graph includes
the same dynamic `past_key.*`/`present_key.*` state family as the staged artifacts, passed
`onnx.checker`, and opened an ONNX Runtime CPU session with 59 inputs and 57 outputs. This is
preparation input only; no native `prepare`→Repo commit or reusable `PreparedModel` has been
observed yet.

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| stage-0-qwen.onnx | 752094335 | `13d8d73c0bf458b2efa2e6a91313fc8e595af81aa9f8c876be0125c040d75984` |
| stage-1-qwen.onnx | 752097486 | `585cce4d4046a07f2d73865c6d47ab194915525c14b0d4e9fd2bb3fb62708dad` |
| canonical-qwen-external.onnx (dynamic KV) | 951819 | `4b41d41cab07f69021bfc3d2c7e9fb6d71aec0554c463acb74cedc611ffbcbff` |
| canonical-initializer.bin | 1503264768 | `413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd` |

The export required a temporary Python 3.8 compatibility overlay and exporter corrections for Qwen3
`head_dim`, cache dtype and PyTorch 2.4 eager attention. Those changes are not production evidence.

### Canonical re-export attempt d01 — exporter harness boundary (2026-09-18)

The first dynamic-KV canonical export command stopped before model loading with
`ModuleNotFoundError: llm_pipeline_lib`; the temporary exporter omitted the
repository `.codex-tmp` module path. Raw log:
`.codex-tmp/spec189-qwen-two-provider-20260918/canonical-dynamic-export.log`.
This is an exporter harness boundary and carries no model or native runtime result.

### Canonical re-export d02 — dynamic state contract (2026-09-18)

The canonical graph was re-exported with 28 `past_key.*`/`past_value.*` inputs and
28 `present_key.*`/`present_value.*` outputs, then externalized to the candidate
paths above. `onnx.checker` and an ONNX Runtime CPU session both passed. The
semantic node mapping was regenerated for the 7,343-node canonical graph and
covers every node exactly once. Export logs are
`.codex-tmp/spec189-qwen-two-provider-20260918/canonical-dynamic-export-d02.log`
and `canonical-dynamic-externalize-d02.log`.

## Five-lane coverage

- production entry/callers: `gap` — native `Runtime::prepare` has not consumed this candidate;
- implementation/wire: `covered` for artifact graph/initializer shape only, Repo publication `gap`;
- test/harness/oracle: `covered` for ONNX checker/CPU session, C++ prepare oracle `gap`;
- build/source closure: `gap` — affected native target not yet rebuilt for Spec189;
- migration/evidence: `covered` for hashes above, run identity still `IN_PROGRESS`.

## Closure decision

`OPEN_FOR_NEXT_BATCH`: implement and run T002/T003 native prepare/Repo path. Export-only output must not be promoted.
