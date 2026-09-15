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

### r61 Rust toolchain probe (2026-09-15)

The host configure initially reached the tokenizer bridge with a Rust prefix
whose `cargo` and `rustc` were rustup shims. They failed without the matching
`RUSTUP_HOME`, after which configure succeeded with
`NDNSF_RUSTUP_HOME=/tmp/rustup-spec186`. Waf now accepts that optional variable
and probes both sealed binaries with `--version` before invoking Cargo, so this
hidden dependency fails fast instead of consuming a full tokenizer build.
Standalone SIF toolchains leave the variable unset.

### r64 current-source receipt (2026-09-15)

After the r61 host build receipt and subsequent documentation changes, a fresh
handoff was prepared from source commit
`2c005662a3822e9a193ed33b2448b6dcfd968815`. The actual local base bytes were
hashed as `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c`;
the rendered definition is
`sha256:cdcfce6c11c6ad7156fb993d61aec67ea9b5e01c9d314447abad6a968462053b` and
the source seal is
`sha256:bf0380c504b604008be5672f990d1f4528d8c4a9f863e409c24fe967088b4e9b`.
`preflight-development-sif.py` returned
`SPEC186_PREFLIGHT_PASS ... workspaceConsumers=11`. No SIF build was started:
the required current-source host qualification manifest is still absent, and
the r61 host bundle is not a matching-container app closure.

The same r64 definition was passed to `build-local-sif.sh
--strict-host-source-seal` with the retained r55 host manifest. It stopped
before base preflight with exit 4 and
`LOCAL_SIF_HOST_GATE_SOURCE_SEAL_INVALID` (`SOURCE_SEAL_REVISION_SOURCE_CHANGE`
for `packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight` and `wscript`).
No SIF output or remote side effect was created.

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

## Source archive consumer cross-check (2026-09-15)

The repository preflight was tightened after the static audit found that a
valid `workspace.tar` could still omit a path consumed by a later builder
command. It now requires the four source archives' entry points, verifies each
`tar -xf` source and `/src` extraction root, scans every non-cleanup `/src/...`
reference in the builder shell, requires the pinned private wheels, and checks
the base-owned `tokenizers`, `onnx`, and `onnxruntime` package directories.
An omitted source member is rejected with
`SPEC186_PREFLIGHT_SOURCE_CONSUMER_PATH_MISSING` before Apptainer build.

The rendered r64 definition and local base were rerun through the expanded
preflight and returned:

```text
SPEC186_PREFLIGHT_PASS wheels=/home/tianxing/NDN/ndn-service-framework/.codex-tmp/spec186-source-handoff-r64/wheels workspaceConsumers=11
```

The focused template suite passed `13 passed`, and the full TigerCluster suite
passed `116 passed in 9.01s`. This remains a pre-build/static closure gate;
the local source-sealed SIF and MiniNDN/Tiger protocol gates are still open.

## r65 current-source preflight (2026-09-15)

After the preflight/skill repair and the current native-closure evidence
commit, the source handoff was regenerated at `01b2230c83fc9c5a81a8b5fb847db6dc3631daf3`.
The new source seal is `sha256:e3ea5a71d92beb4ff7abcbd727a9dcd69a86159eeb989a134621c229f2b1663e`,
and the rendered definition is
`sha256:921add324579d2251c004c32c7e0842ce22ec38efb00db73207ef0935386aa34`.
The builder shell syntax, definition boundary/embedded-Python AST, and expanded
source/base/wheel preflight all passed:

```text
SPEC186_R65_BUILDER_SHELL_SYNTAX_PASS
SPEC186_R65_BOUNDARY_PASS ... pythonSnippets 6
SPEC186_PREFLIGHT_PASS wheels=/home/tianxing/NDN/ndn-service-framework/.codex-tmp/spec186-source-handoff-r65/wheels workspaceConsumers=11
```

No host qualification manifest, source-sealed application bundle, SIF build,
MiniNDN, or Tiger execution is implied by this receipt.

## r70 worker-fixture and base-identity receipt (2026-09-15)

After the full native test campaign exposed the missing worker fixture subtree,
`prepare-local-sif-source.py` was updated and a new handoff was sealed from
source commit `4944c68c477f6f0a44e2f570729c14f34024dd93`. The resulting source
seal is `sha256:f16b2bd98977de50a1996c3192ccf3035c10d1685dfb546ecd4399cc94520056`,
and the rendered definition is
`sha256:232babdee57db42e0bd131c7f4e53e7771ec797c209cbc5ca76991c95100a9a3`.
The actual temporary base bytes were rehashed and bound as
`sha256:071bae8eed882bf865f4f5f6bf06c900f17a93e42bac870001a331c9e2cc82c2`;
the previous `1dd962...` identity was rejected and retained as a failure.

`prepare-development-handoff.py prepare` and `verify` returned `SOURCE_READY`,
`render` returned the above definition identity, and the expanded preflight
returned:

```text
SPEC186_PREFLIGHT_PASS wheels=/home/tianxing/NDN/ndn-service-framework/.codex-tmp/spec186-source-handoff-r70/wheels workspaceConsumers=11
```

The r70 archive contains `tests/standalone/spec182-worker-tools` and is the
first source handoff whose archive producer, Waf target census and preflight
contract agree on the complete worker subprocess test closure. This remains a
static/source-delivery PASS; no SIF or Tiger qualification is claimed.
