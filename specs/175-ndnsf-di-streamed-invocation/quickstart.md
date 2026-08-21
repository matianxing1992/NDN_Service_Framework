# Spec 175 Validation Quickstart

This is the only planned execution route. Commands labeled **planned** do not
exist until their owning task implements and tests them. Do not replace them
with an ad hoc runner and do not advance past a failed gate.

## 0. Confirm authority and branch

```bash
cd /home/tianxing/NDN/ndn-service-framework
test "$(git branch --show-current)" = Experimental
test "$(jq -r .feature_directory .specify/feature.json)" = \
  specs/175-ndnsf-di-streamed-invocation
git status --short
```

The worktree may contain unrelated user documentation/evidence. Never reset,
clean, implicitly stash, or use `git add -A`. Every gate manifest lists in-scope
dirty inputs and excludes unrelated paths.

## 1. G0 contract gate

**Planned**:

```bash
python3 scripts/spec175_contract_gate.py \
  --feature specs/175-ndnsf-di-streamed-invocation \
  --output results/spec175/g0/qualification-manifest-v1.json
```

Expected: `PASS`, no placeholders/TLV collisions/default drift/runtime
Transformers dependency, and complete FR/SC/source/test mapping.

## 2. Configure and build one host toolchain

```bash
./waf configure --with-tests
./waf build -j4
```

Do not mix Linuxbrew and system linker/library closures. Record the configure
command, compiler/linker, Boost, ndn-cxx, NDN-SVS, ORT, Python, and `ldd` roots.

## 3. G1 unit gate

```bash
build/unit-tests --log_level=test_suite

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_streamed_invocation_api.py \
  tests/python/test_spec175_cpu_fixture.py \
  tests/python/test_spec175_evidence.py
```

Expected: complete native binary and named Python cases pass with zero required
skip; unary, Targeted, security, StreamFacade, and Spec 174 regressions pass.

## 4. G2 CPU integration gate

**Planned**:

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I14 \
  --healthy-repeats 3 \
  --seed 1750001 \
  --output results/spec175/g2
```

Expected: every case in
[validation-contract.md](contracts/validation-contract.md) has one manifest row,
all exact oracles pass, no accepted event is dropped or delivered twice, and all
processes terminate cleanly.

## 5. G3 build and preflight the exact local SIF

Freeze one candidate file before building:

```bash
export SPEC175_CANDIDATE_MANIFEST="$PWD/results/spec175/candidate.json"
```

**Planned interfaces extending current local-SIF tooling**:

```bash
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST"

packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --output results/spec175/g3/qualification-manifest-v1.json
```

Expected: `PROMOTABLE`, one SIF digest, Python 3.10 native import, closed
RPATH/`ldd`, Boost 1.71 and NDN-SVS `Experimental`, real workload `--help`,
CPU/CUDA ORT inventory, `tokenizers` present, and deployed PyTorch/Transformers
absent. Build the Python extension inside the candidate SIF/sealed builder only.

## 6. G4 exact-SIF CPU MiniNDN gate

```bash
sudo -E env \
  SPEC175_RUN_REAL_MININDN=1 \
  SPEC175_CANDIDATE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
  PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_spec175_real_minindn_gate.py
```

Expected: M01-M09 each pass in three clean processes (27/27), exact tiny-model
tokens/final results, real controller/NFD/SVS/role processes, complete Interest/
Data lineage, and clean process-tree teardown. Stop here on any failure.

## 7. Review promotion manifest

Before Tiger, verify the candidate rather than selecting values interactively:

```bash
jq '{status,git,sif,model,tokenizer,workload,lowerGates}' \
  "$SPEC175_CANDIDATE_MANIFEST"
sha256sum "$(jq -er .sif.path "$SPEC175_CANDIDATE_MANIFEST")"
```

Required: exact hash equals the manifest and every lower gate is `PASS` or
`PROMOTABLE` as specified. A dirty candidate must include an explicit source
seal and scoped diff; unrelated dirty files are not copied.

## 8. G5 Tiger single-GPU functional gate

**Planned**:

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --gate single-gpu
```

Expected: frozen Qwen3-0.6B CUDA ONNX subject, six measured exact invocations,
one End/Response each, no CPU fallback.

## 9. G6 Tiger three-Provider functional gate

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --gate multi-provider
```

Expected: pinned Qwen3.6-27B three-role split `[0,21)`, `[21,42)`, `[42,64)`,
one GPU/Provider/role, six measured exact invocations, one plan/prefill each,
complete activation/feedback/event lineage, zero fallback.

## 10. G7 Tiger performance qualification

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --gate performance
```

Expected: three independent processes, one excluded cold plus ten warm
invocations each, 30 warm units total. The summary assigns exactly one verdict:

```text
PERFORMANCE_PASS
FUNCTIONAL_PASS_PERFORMANCE_MISS
FAIL
```

`PERFORMANCE_PASS` requires exact correctness, zero fallback, median warm
steady-state >=20.0 token/s, and p95 inter-token <=75 ms.

## 11. Completion check

The feature is implemented only when:

```text
G0 PASS -> G1 PASS -> G2 PASS -> G3 PROMOTABLE ->
G4 PASS -> G5 PASS -> G6 PASS -> G7 verdict recorded
```

`implemented`, `compiled`, `one unit suite passed`, `SIF built`, `model loaded`,
or `GPU used` are intermediate evidence levels, not completion.
