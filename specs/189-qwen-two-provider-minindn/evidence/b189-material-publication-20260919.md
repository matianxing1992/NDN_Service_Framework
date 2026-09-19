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
