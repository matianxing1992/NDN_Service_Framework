# Spec186 static build-preflight repair — 2026-09-15

## Finding

Static review of `build-local-sif.sh` found that the `Bootstrap: localimage`
`From:` path was resolved after `preflight-development-sif.py` ran. The build
entry therefore passed no `--base-sif` argument and silently skipped the
read-only base capability and NumPy import checks for layered candidates.

The same review found that `%files` proved only that `workspace.tar` existed;
it did not prove that source subtrees later consumed by builder `cp` or pip
commands were present in that archive. A missing replay helper could therefore
survive until `%post` after compilation had begun.

## Repair

- Resolve and hash the definition's absolute `localimage` base before running
  the development preflight.
- Extract `/src/ndnsf/...` operands from builder `cp` and pip commands and
  require each consumed path or subtree in `workspace.tar`.
- When a base SIF is provided, execute the rendered builder's base-owned
  `test -x`, `test -f`, and `test -d` predicates in a read-only Apptainer
  execution. This catches missing compilers, ONNX SDK files, Rust/Cargo and
  system headers before native compilation.
- Add `build-local-sif.sh --verify-existing` to re-run label, source, hash and
  Spec175 runtime gates against an existing SIF without recompilation. The
  record states `local-apptainer-existing-sif-verify`; changed candidate inputs
  still require a new output path.
- Record the defect and lesson in `docs/failure-log.md` so a future repair does
  not restore the weaker ordering.
- The capability parser now expands absolute prefixes exported by the builder
  shell before reading `test -x/-f/-d` predicates, so Rust/Cargo variable-backed
  paths are checked rather than silently omitted.

The capability scope is now explicit: only predicates between
`SPEC186_BASE_CAPABILITY_BEGIN/END` are executed against the input base. APT
installed compilers and system development packages remain post-install checks
inside the builder, so a base that intentionally relies on the recipe's APT
bootstrap is not rejected before that bootstrap runs.

## Dependency boundary audit

The source/build review also confirmed that the repeated dependency failures
are partly a real Waf coupling and partly packaging drift:

- `ndn-service-framework` has a Core link closure of NDN-CXX, NDN-SVS, Boost,
  Protobuf, NAC-ABE, NDNSD, OpenSSL and `libdl`.
- The installable `ndnsf-distributed-inference` target currently combines DI
  mechanism, ONNX, YOLO and Qwen sources. Its shared variant adds the static
  Rust tokenizer bridge.
- `configure()` requires the ONNX full-protobuf prefix and invokes the Rust
  bridge builder without an independent profile guard. The assembly worker in
  `examples/wscript` is declared before the `WITH_EXAMPLES` return and consumes
  ONNX/ONNX Runtime as well.
- Python package metadata separates Core/SDK, ONNX (CPU/GPU optional Runtime)
  and Qwen (`tokenizers`), but those package boundaries do not yet select the
  native Waf targets.

The portability defect is now closed at the source boundary: Waf requires
`NDNSF_RUST_PREFIX` and `NDNSF_CARGO_HOME` explicitly and defaults only the
tokenizer target to `build/tokenizer-bridge-target`; it no longer falls back to
`.codex-tmp/spec182-t001-dependencies`. The full source/target table and the
profile split proposal are recorded in
`Experiments/TigerCluster/docs/dependency-boundaries.md`. An actual Core-only
profile remains future work and must be implemented as a new SpecKit task with
a clean rebuild; no dependency check was removed in this repair.

## Verification

```text
python3 -m pytest -q \
  Experiments/TigerCluster/tests/test_development_runtime_template.py \
  Experiments/TigerCluster/tests/test_spec186_candidate.py
55 passed in 7.38s
```

The tests cover shell syntax, source-consumer missing-path rejection, complete
consumer enumeration, base-capability extraction, template boundaries and
the preflight-before-build ordering. This is a static/build gate result only;
it does not qualify MiniNDN, CUDA, single-node Tiger, two-node Tiger or Qwen.

## r61 marker-scoped preflight receipt

The current-source handoff was regenerated at commit
`c4c908e8f43aa5da9780d3423388de9583daea98`. The rendered definition digest is
`sha256:91f0c4e65eb30624c518c7352c50c6ae294e54b50365f200a341e4e5ed548d28`
and its source seal is
`sha256:d6f31b85e8bbceb04f802511aeb20630dd835364e39eefedad5db5cb6cdfd9b7`.
Against the exact local base `spec186-repaired-base-final.sif` (SHA-256
`eb0c2760e623aca7b844fca97dc470527c0ef74d0d97f3794ff8fa4a7cbd1d99`), the
preflight returned:

```text
SPEC186_PREFLIGHT_PASS wheels=/home/tianxing/NDN/ndn-service-framework/.codex-tmp/spec186-source-handoff-r61/wheels workspaceConsumers=11
```

The read-only base predicates covered the four ONNX SDK files, Cargo registry
source directory, and the exported Rust `cargo`/`rustc` paths. The APT-provided
Clang 10 predicate was correctly excluded from the base list. This is a static
preflight PASS only; it does not imply SIF build, MiniNDN, CUDA, or Tiger
qualification.

## r61 strict host-gate check

The build entry was invoked with `--strict-host-source-seal` and the
retained r55 host manifest. It stopped before base preflight with
`LOCAL_SIF_HOST_GATE_SOURCE_SEAL_INVALID
code=SOURCE_SEAL_REVISION_SOURCE_CHANGE`, identifying the changed preflight
helper and `wscript`. No SIF compilation started; r55 remains historical
tiny-ONNX evidence and a new host-gate receipt is required for r61.
