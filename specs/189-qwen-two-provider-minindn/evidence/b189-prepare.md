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

## Prepare-time Repo boundary r1 — 2026-09-18 09:25 -0500

The production `Runtime::prepare` path now accepts a separate
`RepositoryArtifactPublisher`. When configured, it publishes through the Repo
owner during preparation and stores the immutable receipt in the prepared
package; request execution does not call the publisher. The Runtime passes the
registered `modelKey`, and the catalog exposes the publication options needed by
the Repo owner. The existing Core publisher remains the fallback when no Repo
publisher is configured.

`RepoSourceProvider` implements this boundary for canonical source,
initializer and root-manifest objects. A cold call commits the source/root
objects; a second call is a serialized, identity-checked hot lookup. The hot
path re-reads source, initializer and root ranges and recomputes their digests,
so a manifest/payload mismatch is rejected. Durable Repo receipts set
`rollbackOwned=false`; a later package or cache failure cannot delete objects
that another prepared handle may already reuse. Non-empty
`layerManifestDigests` are rejected until a layer-payload owner is connected;
they are not advertised as published layers.

The C++ selector in `tests/unit-tests/spec189-repo-publication.t.cpp` covers:

- cold commit → hot hit and stable names/digest;
- hot receipt rollback preserving durable objects;
- source payload corruption and model-key conflict rejection;
- explicit rejection of an unconnected layer reference; and
- cancellation before writes with an empty Repo catalog.

The unit target source closure explicitly includes the RepoCore, RepoClient,
RepoNode and filesystem backend definitions. Immutable static review v3 passed
(`.codex-tmp/spec189-repo-publication-review-v3/manifest.sha256`); the fixture
permission correction (private `0700` root) passed v4 review
(`.codex-tmp/spec189-repo-publication-review-v4/manifest.sha256`).

Validation from the repository root, using the canonical global dependency
install and build tree `build-spec189-b189-3-global-r3/`:

```text
../waf build --targets=unit-tests -j2       PASS (r3 2m32.904s; r4 26.037s)
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite  PASS (2 cases)
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite  PASS
```

The first Repo selector attempt was a fixture boundary (`repo-file-root-not-private`)
and is preserved in `.codex-tmp/spec189-b189-3-unit-repo-r3.log`; the corrected
run is `.codex-tmp/spec189-b189-3-unit-repo-r4.log`. The first Runtime invocation
from inside the build directory could not find the repository-relative YOLO
oracle; it was rerun from the repository root and passed. These are focused C++
checks only. They do not prove Qwen layer publication, payload-free two-provider
Selection, Provider execution, hidden-state handoff, MiniNDN completion or
`QWEN_TWO_PROVIDER_PASS`.

## Five-lane result and closure decision

- production entry/callers: **covered** for the prepare-time Repo publisher;
- implementation/wire: **PARTIAL** — canonical source/initializer/root only;
  layer payload publication remains an explicit dependency;
- test/harness/oracle: **covered** by the two C++ selectors above;
- build/source closure: **covered** by the global `unit-tests` build and explicit
  Repo source registration;
- migration/evidence: **PARTIAL** — no real Qwen native candidate has consumed
  the layer references.

**Closure**: T003 remains **PARTIAL**. The stable boundary is now safe and
observable, but the batch cannot advance until a real layer-payload owner is
connected and its C++ cold/hot publication evidence is added.

## Prepare-time Repo boundary r2 — 2026-09-18 09:45 -0500

The v5 static review found three lifecycle defects in the outer preparation
commit path: a superseded normal job could return a package after rolling back
its receipt; cache-hit rollback called an external owner while holding the
cache mutex; and receipt construction after cache insertion could roll back a
publication already referenced by the cached package. The repair keeps the
superseded live result committed, releases the cache mutex before external
rollback, and marks the publication committed immediately after cache
accounting succeeds. The refresh result copies its package before the commit
mark so a constructor exception remains rollback-safe.

Immutable snapshot `.codex-tmp/spec189-repo-publication-review-v7/` passed the
official read-only review-agent (`STATIC_PASS`); its manifest matched all
files and no P0/P1/P2 finding remained. The review did not run build, ASan,
TSan or dynamic cancellation/exception stress selectors.

Using the canonical global dependency installation and existing build tree:

```text
../waf build --targets=unit-tests -j2  PASS (27.326s; r5)
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite  PASS (2 cases; r5)
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite  PASS (r5)
```

Raw logs: `.codex-tmp/spec189-b189-3-unit-build-r5.log`,
`.codex-tmp/spec189-b189-3-unit-repo-r5.log` and
`.codex-tmp/spec189-b189-3-runtime-repo-r5.log`. This is focused C++
validation only. Layer-payload publication, real Qwen preparation, two-provider
Selection, Provider execution, hidden-state handoff, MiniNDN completion and
`QWEN_TWO_PROVIDER_PASS` remain unobserved.

## Five-lane result and closure decision r2

- production entry/callers: **covered** for the prepare-time Repo publisher and
  outer cache commit/rollback boundary;
- implementation/wire: **PARTIAL** — canonical source/initializer/root only;
  layer payload publication remains fail-closed;
- test/harness/oracle: **covered** by the two Repo/Runtime C++ selectors above;
- build/source closure: **covered** by the explicit Repo source closure and
  successful `unit-tests` build;
- migration/evidence: **PARTIAL** — no real Qwen native candidate has consumed
  layer references.

**Closure**: T003 remains **PARTIAL**. The corrected lifecycle boundary is
  locally verified, but the batch still lacks a connected layer-payload owner
  and full Qwen preparation evidence.
