# GitHub GPU assembly run 29309152207 verdict

**Verdict**: `EXECUTED_FAIL`; this candidate identity is frozen and MUST NOT be
rerun. T174's exactly-once dispatch passed, but T175 produced no final runtime
digest. T160 did not run, and this is not SIF, Slurm, CUDA, or Qwen evidence.

## Immutable identity

| Field | Value |
|---|---|
| Workflow run | `29309152207` (run 9, attempt 1) |
| URL | `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/29309152207` |
| Event | `workflow_dispatch` |
| Source revision | `94fc25062aa7fced301b1f9db983de3e9a8910e3` |
| Source ref | `spec110-gpu-source-94fc25062aa7fced301b1f9db983de3e9a8910e3` |
| Release identity | `spec110-runtime-94fc25062aa7fced301b1f9db983de3e9a8910e3` |
| Foundation input | `ghcr.io/matianxing1992/ndnsf-di-foundation@sha256:d2aaca7b18aa56b9e24ac6b9c6c9c6a98a26117c32d70966845ae1589ea62d15` |
| Dispatch accepted | `2026-07-14T05:40:37Z`, HTTP 204 |
| Terminal state | `completed/failure`, `2026-07-14T06:00:18Z` |
| Retry policy | `NO_AUTOMATIC_RERUN` |

## T174 dispatch gate

The read-only precheck proved the workflow was active, the new source and
release tags did not exist, no prior run used the new release identity, every
older source/release identity remained frozen, and the anonymously readable
Foundation manifest matched its reviewed local image. The annotated source tag
dereferences exactly to the reviewed source revision.

A crash-safe `INTENT_DURABLE` journal was fsynced before one dispatch API POST.
The API returned HTTP 204, and reconciliation found exactly one run with the
expected source ref and SHA. The ignored operational journal is retained at
`results/spec110-itiger-qwen-live/github-dispatch-t174/dispatch-record.json`.
No automatic rerun occurred or is permitted.

## Measured terminal failure

Preflight, the source secret scan, Buildx setup, and registry login passed. The
GPU assembler compiled and installed the three workspace Python packages, then
failed in its final runtime-closure scan:

```text
RuntimeError: RUNTIME_LIBRARY_MISSING:/opt/venv/lib/python3.10/site-packages/pillow.libs/libwebpmux-f0bc54e2.so.3.1.1
```

The exact Pillow 11.1.0 manylinux wheel contains the required hashed sibling
`libwebp-0feb04d2.so.7.1.10`. `readelf -d` on `libwebpmux` shows that NEEDED
entry and no RPATH/RUNPATH. Default `ldd` therefore reports the sibling as not
found, while the same command with only the wheel's `pillow.libs` directory in
`LD_LIBRARY_PATH` resolves it and its bundled `libsharpyuv`. The wheel digest
used for the local reproduction was
`sha256:abc56501c3fd148d60659aae0af6ddc149660469082859fa7b066a298bde9482`.

This is a scanner false negative about a complete vendored wheel bundle, not a
missing system `libwebp` package. The generic repair must allow an ELF to resolve
its own sibling bundle directory and must still fail when that sibling is
actually absent. A Pillow-specific exception or ignored `not found` result is
forbidden.

The runner had 89 GB free before the build and 62 GB afterward, excluding disk
pressure. The final runtime tag is absent. Release manifest, signature, SBOM,
anonymous final-digest proof, SIF materialization, Slurm allocation, CUDA
provider execution, and Qwen inference were skipped or never reached.

## Preserved evidence

The ignored evidence directory is
`results/spec110-itiger-qwen-live/github-dispatch-t174/`.

| Artifact | Digest |
|---|---|
| Text run log, 685039 bytes | `sha256:3e1846a641625e78cc0bd4c0f15607133c0cfa3ead937c674edc721274a169fe` |
| Complete log archive, 174521 bytes | `sha256:4b0e3e344fab779126d6e5d4af2ae2df8ffcf9fd99043807ff44da9f5006a97b` |
| Buildx record, 132026 bytes | `sha256:02c3278b592b6e05530f1d44979533a376c01c28b7593592d4a805294bd27a8d` |
| Release-evidence archive, 1829 bytes | `sha256:de3d0f53b2fae21f29d0c14ca4cb342369d89ac60f526761fd772e481404becd` |

Preflight recorded eight locked sources, 38 Python packages, and 11 CUDA system
requirements. The source scan covered 79 files and 249454 bytes with zero
findings. These passing preliminary checks do not override terminal failure.

## Replacement boundary

Run 9, its source tag, Foundation digest, and release identity are immutable
negative evidence. T176 closes by preserving the T160 blocker and making no
iTiger submission. Any repair must pass a real-ELF sibling-present and
sibling-missing regression, use a new committed source identity, create a new
source seal and source-bound Foundation digest, and obtain fresh explicit human
authorization before one new dispatch. T160 remains locked until a later
replacement produces a final immutable GHCR digest.

## T178-T179 audited local repair

The RED real-ELF test compiled a consumer and a SONAME-bearing sibling shared
library without RPATH/RUNPATH. Before the repair, the sibling-present test raised
`RUNTIME_LIBRARY_MISSING`, while the sibling-deleted negative test already
failed closed as required.

`linked_paths()` now invokes `ldd` with only the scanned ELF's resolved parent
directory prepended to the inherited `LD_LIBRARY_PATH`. It neither adds another
bundle directory nor ignores `not found`; deleting the required sibling still
raises the same fail-closed error. The behavior is generic and contains no
Pillow/package-name exception.

The repaired local gates passed:

- 18/18 focused runtime-closure and sealed-workflow tests;
- 101/101 complete Spec 110 Python tests across 19 test files;
- all five offline integration scripts, including release/materialization,
  rootless fallback, and six runtime-compatibility cases;
- GPU build preflight with eight sources, 38 Python packages, and 11 system CUDA
  requirements;
- source scan of 109 files and 372518 bytes with zero findings;
- Python compilation, JSON validation, and `git diff --check`;
- synchronized CodeGraph coverage for `linked_paths()` and both real-ELF tests;
  and
- strict Spec Kit structure with 37 FRs, 13 SCs, six stories, and 180 tasks.

The post-implementation audit verdict is `PASS` for this local repair. These
results prove only the scanner behavior and build contract; they do not upgrade
run 9 or claim that a final OCI, SIF, CUDA runtime, or Qwen inference exists.
