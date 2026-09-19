# B189-1b material-backed publication checkpoint — 2026-09-19

## Scope

This checkpoint covers the material-backed canonical publication repair found
at the first real Qwen preparation boundary. The NDNSF Waf target builds only
the NDNSF DI/Core/Repo-owned sources and test selectors. NAC-ABE is an
installed external SDK built by the NAC-ABE project Waf; no NAC-ABE source was
added to the NDNSF target or rebuilt here.

The repair keeps the prepare/request contract topology-independent: a
material-backed prepare publishes the root, material manifest, and material
payloads, while the full source and initializer remain identity metadata and
are not published as duplicate whole-model objects. The publication budget is
separate from the per-role assembly budget and is preflighted before the first
transport write.

## Review gate

The immutable r5 review snapshot was
`.codex-tmp/spec189-material-publication-review-r5/scoped.diff` with SHA-256
`77cf06ab8ad1420c76b623f7f2b0d4b8c4b49832539f4aaafc6daa74a40a52b7`.
The official read-only review-agent returned `STATIC_PASS` after checking the
five lanes: caller/runtime receipt binding, state/ownership and rollback,
concurrency/cancellation boundaries, compatibility/build registration, and
test/evidence criteria. It explicitly left the 16 MiB root reserve as a
candidate-specific bound rather than a universal transport proof.

## Compile/link

From `build-spec189-b189-3-global-r3/`, using the system-first compiler path
and `-j4`:

```text
python3 ../waf build --targets=ndnsf-distributed-inference,spec189-prepared-request,
spec189-placement-oracle,spec189-two-provider-oracle,
spec189-provider-stage-oracle-tests,spec189-cli-oracle-tests,
spec189-material-oracle-tests -j4
```

completed in 4m51.374s. The new Waf-registered target
`spec189-canonical-publisher` then completed in 2m09.773s with the complete
DI/ONNX/YOLO/Qwen source closure. The target is registered in
`tests/wscript` beside the production Spec189 selectors and uses the same
framework includes, global dependency use list, and RPATH.

## C++ runtime selectors

Run from the repository root with the configured build tree in
`LD_LIBRARY_PATH`:

| Selector | Result |
| --- | --- |
| `Spec182CanonicalPublisher/MaterialBackedPublicationOmitsWholeModelAndPreflightsUnionBudget` | 1/1 PASS |
| related publisher regressions (`PreparedReceiptIndex...`, `CatalogComposes...`, `PublishesOwnedInline...`, `RejectsSource...`, `RejectsInvalidCoreReceipts...`) | 5/5 PASS |
| `spec189-prepared-request` protected reference/publication cases | 2/2 PASS |
| `spec189-placement-oracle` | 1/1 PASS |
| `spec189-provider-stage-oracle-tests` | 18/18 PASS |
| `spec189-material-oracle-tests` | 15/15 PASS |
| `spec189-cli-oracle-tests` with `spec189-two-provider-oracle` | 5/5 PASS |

The full `spec189-canonical-publisher` suite has four failures in older
fixtures: two cancellation/source-lifetime cases, one cancellation-after-
source case, and one encrypted-publication test using a root-owned temporary
directory. Those failures are retained as fixture/environment boundaries and
are not counted as a product PASS or as evidence against the new material
publication assertion. The focused new test and its related legacy cases do
not exercise those failing fixtures.

## Real-run boundary

Before this repair, real MiniNDN run r38 reached Controller, Authority and both
Providers, then stopped at the first requester production boundary:
`PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`. It produced no
ACK, Selection, Provider assembly, execution, terminal response, or
qualification verdict. A new run with the repaired candidate is still
required; this checkpoint does not claim MiniNDN or Qwen two-provider PASS.

## Four miss classes

| Lane | Result |
| --- | --- |
| static | r5 official `STATIC_PASS` |
| compile-link | affected DI closure and registered publisher selector passed |
| runtime-test | focused C++ publication, protected request, placement, and causal oracles passed |
| unobserved | real protected ingress after the repair, Qwen preparation, ACK/Selection, two-provider execution, output, drain, and repeat |

The next exit is a fresh run id with the repaired publication configuration,
after the updated DI library and build receipt are installed. The retained r38
raw run is not reused.

### r39 retry after the publication-budget repair

The fresh run `spec189-v39-cpp-material-budget` used the updated global-r3
receipt and current binary hashes. It reached Controller, Authority and both
Providers, and it no longer emitted `DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`.
The requester then spent the configured preparation window in the real Qwen
preparation path and terminated with:

```text
NATIVE_REQUEST_STAGE_FAILED code=PREPARATION_TIMEOUT boundary=preparation
PREPARATION_TIMEOUT domain=local boundary=preparation message=DI_NATIVE_PREPARATION_TIMEOUT
```

The supervisor receipt records `cleanup=PASS`, `boundary=null`, and return code
1. The first production boundary is therefore now a preparation-time budget,
not the former publication-byte rejection. No ACK, Selection, material fetch,
assembly, runner, execution, terminal response, or qualification verdict was
observed. Raw logs and resource samples remain under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r39/`.
The next retry must first explain or reduce this preparation cost; changing
only the run id or timeout would not establish a product fix.

## 2026-09-19 r19/r20 local C++ verification

The focused integration fixture was corrected after the first positive bundle
run exposed `DI_NATIVE_ONNX_GRAPH`: the earlier test selected only nodes 0 and
1, which cannot produce the declared role output. The final fixture derives a
source with the role-0 nodes 0–20 plus one valid unreachable `Identity` node at
index 21. The worker therefore checks a valid selected role while the receipt
contains a real unselected material payload. Snapshot r19 received official
read-only `STATIC_PASS` with SHA-256
`92d475dfc3c1bf8a77150c52ac9696dccf4c7ea7d101635f996bceb28e416c6a`.

The affected integration target rebuilt through the repository-root Waf tree
with `-j4` in 22.803s. From the repository root, with
`NDNSF_SPEC182_BIN_DIR=build-spec189-b189-3-global-r3`, the following real C++
selectors passed:

| Selector | Result |
| --- | --- |
| `Spec175NativeAssembly/Spec189MaterialConsumerFetchesOneSelectedBundle` | PASS |
| `Spec175NativeAssembly/Spec189MaterialConsumerBoundsSelectedPayloadFetches` | PASS |

The positive selector used the real `DI_NativeOnnxAssemblyWorker`; it fetched
the manifest, authenticated receipt, and selected bundle once, did not fetch
the unselected bundle, and passed receipt-identity and aggregate-budget
negative paths.

The first Repo publication rerun exposed a test-contract error rather than a
production failure: `RepoSourceProvider` intentionally commits one bounded,
independently addressable range-store object per material payload, while the
protected NDN publisher is the layer that coalesces payloads into bundles. The
assertion was corrected and snapshot r20 received read-only `STATIC_PASS` with
SHA-256 `448a01c7e7848b46bd23caba73fcac3e7cd10a6ebd7f5466eaa1bf95cbb41aa1`.
The unit target rebuilt through root Waf `-j4` in 26.617s. These selectors then
passed from the repository root:

| Selector | Result |
| --- | --- |
| `Spec189RepoPublication/MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption` | PASS |
| `Spec182GrantIssuer/LegacyInlineRootAboveAuthorityCapIsRejectedBeforeRequestTransport` | PASS |
| `Spec182CanonicalPublisher/MaterialBackedPublicationOmitsWholeModelAndPreflightsUnionBudget` | PASS |

This evidence confirms the local C++ material publication and selected-bundle
boundaries only. It does not establish the real protected Qwen prepare,
ACK/Selection, two-provider assembly/execution, terminal output, cleanup
drain, MiniNDN or Tiger qualification. The root NDNSF Waf compiles only
NDNSF-owned Core/Repo/DI/examples/tests; NAC-ABE is an installed external SDK
built by the NAC-ABE project build system and is not recursively built by the
NDNSF Waf graph.
