# Spec 175 Validation Quickstart

This is the only planned execution route. Commands labeled **planned** do not
exist until their owning task implements and tests them. Do not replace them
with an ad hoc runner and do not advance past a failed gate.

**Cost boundary:** execute only G0-G3 while the implementation is changing.
Do not build, preflight, upload, or replay a SIF, and do not request Tiger
resources, before the real host/CPU MiniNDN M01-M14 matrix passes 42/42. Any
older SIF is diagnostic-only. Once G3 passes, freeze the manifest, build one
final SIF, replay the same manifest, and then promote that exact hash to Tiger.

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

```bash
python3 scripts/spec175_contract_gate.py \
  --feature-dir specs/175-ndnsf-di-streamed-invocation \
  --output results/spec175/g0/qualification-manifest-v1.json
```

During development the retained expected-negative run is `BLOCKED_EXPECTED`.
The current accepted G0 record is
`results/spec175/g0/qualification-manifest-final-20260825.json`; it has no
placeholders/TLV collisions/default drift/runtime Transformers dependency, and
records the service-only Normal API, shared collaboration owner,
selected-Provider-only event grants, complete 58-FR/12-SC mapping, and the
current source seal.

## 2. Configure and build one host toolchain

```bash
./waf configure --with-tests --toolchain-root=/usr/bin \
  --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs \
  --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
./waf build --target=unit-tests -j2
ldd build/unit-tests
```

Do not mix Linuxbrew and system linker/library closures. The configure probe
must link `subscribeToProducerWithCatchUp`; `ldd` must resolve NDN-SVS to the
explicit build tree and Boost to 1.71. Record the command, compiler/linker,
Boost, ndn-cxx, NDN-SVS, ORT, Python, and `ldd` roots.

The current host Python 3.8 diagnostic extension imports against the current
framework build. Rebuild and recheck its hash/`ldd` whenever the native ABI
changes. It is diagnostic-only and must never be copied into the Python-3.10
candidate SIF; that extension is built inside the candidate/sealed builder.

## 3. G1 unit gate

```bash
build/unit-tests --log_level=test_suite

python3 scripts/run_spec175_python_gate.py \
  --source-seal results/spec175/g0/source-seal-final-20260825.json \
  --output results/spec175/g1/qualification-manifest-final-20260825.json
```

Expected: complete native binary and the bounded Spec175/shared-contract Python
subject pass with no unexpected skip; unary, Targeted, security, StreamFacade,
and Spec 174 regressions pass. Provider-session state tests must prove automatic
prefill commit, exact later lookup/atomic commit, pin/evict/release, and no
zero-state decode fallback through the production execution call. The full
historical Python tree is diagnostic, not a reason to rebuild a SIF.

## 4. G2 CPU integration gate

The final fail-closed runner registers native I01-I20. Reproduce the
qualified healthy subset with:

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I03,I15-I18 \
  --healthy-repeats 3 \
  --seed 1750001 \
  --source-seal results/spec175/g0/source-seal-final-20260825.json \
  --output results/spec175/g2/qualification-manifest-final-20260825.json
```

The complete gate remains:

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I20 \
  --healthy-repeats 3 \
  --seed 1750001 \
  --source-seal results/spec175/g0/source-seal-final-20260825.json \
  --output results/spec175/g2/qualification-manifest-final-20260825.json
```

Current historical registered-subset result: I01-I15 pass, every selected Provider
coordinator terminates cleanly, and I15 changes the role map. I13 is the
default-disabled live-transport boundary case. The historical pre-Face-fix
27-process matrix is retained separately; the current 27-process G2 manifest
predates the automatic Provider-state/no-manual-feedback correction and is now
historical only. The replacement G2 manifest must additionally record one
prefill state commit, expected per-role decode hits/commits/cleanup, zero
unexpected misses, and zero decode-state dependency bytes for I01-I03/I15; the
eight-token oracle requires one prefill commit plus seven decode hits/commits per
role. A G2
manifest without those fields, the source-seal digest, or exact integration-
binary SHA-256 is diagnostic-only.

Final expectation: every case in
[validation-contract.md](contracts/validation-contract.md) has one manifest row,
all exact oracles pass, no accepted event is dropped or delivered twice, and all
processes terminate cleanly. I15 must change the role map when the signed
Provider capability/residency assignment is permuted; a fixed map is failure.
I16-I20 additionally require exact two-turn continuation, a fresh Request and
generation per turn, delta-prefill oracle parity, transactional all-role
checkpoints, bounded host-tier emulation, explicit fallback/failure, and
single-writer conflict handling. They are planned work, not part of the current
historical G2 pass.

## 5. G3 host/CPU MiniNDN gate (before SIF)

Run the four-Provider M01-M14 matrix against the current native/Python build
and the checked-in tiny ONNX fixture. This is the primary low-cost debugging
boundary; do not build or upload a SIF before it passes.

Expected: M01-M14 pass in the registered clean processes with real NFD/SVS/ABE,
bounded queues, packet lineage, and clean teardown. A failure returns to the
native or MiniNDN owner and does not trigger a SIF rebuild.

Before declaring G3 promotable, enumerate every process started for the current
source/subject, not only the three selected PASS rows. Any signal exit, core
dump, missing case result, or unclassified native return code blocks G3 even if
an identical rerun passes. The current M09 `returncode=-11` record in
`evidence/t022-g3-current-20260825n.md` must be classified and covered by a
focused regression, then the affected triplet and source-bound manifest must be
regenerated. The current host/CPU checkpoints already exercise M11-M14 for
conversation continuation, three-conversation state-tier isolation,
mismatch/restart fallback, and concurrent-parent/cancel behavior; they still
need to be repeated under the current source seal as part of the 42/42 matrix.
Do not build a SIF from the historical selected 30/30 manifest; the expanded
gate requires 42/42 and no unclassified process exit.

## 6. G4 final local SIF build, preflight, and replay

Freeze one candidate file before building:

```bash
export SPEC175_CANDIDATE_MANIFEST="$PWD/results/spec175/candidate.json"
export SPEC175_HOST_GATE_MANIFEST="$PWD/results/spec175/g3/host-minindn-manifest.json"
export SPEC175_LOCAL_ROOT="$PWD/.local-tmp/spec175-final-candidate-<id>"
export SPEC175_DEFINITION="$SPEC175_LOCAL_ROOT/spec175-runtime-final.def"
export SPEC175_SIF="$SPEC175_LOCAL_ROOT/spec175-runtime.sif"
export SPEC175_BUILD_RECORD="$SPEC175_LOCAL_ROOT/build-record.json"
export SPEC175_SOURCE_SEAL="$SPEC175_LOCAL_ROOT/source-current/source-seal.json"
```

**Required split preflight before promotion** (the corrected local builder
enforces the sealed G3 manifest; MiniNDN remains a host dependency):

```bash
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --definition "$SPEC175_DEFINITION" --sif "$SPEC175_SIF" \
  --record "$SPEC175_BUILD_RECORD" --source-seal "$SPEC175_SOURCE_SEAL" \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --strict-host-source-seal --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --expected-apptainer 1.5.3

packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight \
  --sif "$SPEC175_SIF" --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --source-seal "$SPEC175_SOURCE_SEAL" \
  --workload packaging/ndnsf-di-container/jobs/spec175/workload.json \
  --output results/spec175/g4/sif-runtime-preflight.json

packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --repository-root "$PWD" \
  --topology-file "$PWD/Experiments/Topology/spec175-host-gate.conf" \
  --replay-driver "$PWD/packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py" \
  --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --expected-apptainer 1.5.3 \
  --output results/spec175/g4/host-substrate-preflight.json
```

Run this section only after G0-G3 pass; running it earlier is a workflow error.
The candidate reports `READY_FOR_G4_REPLAY` only after one corrected SIF
digest, a passing native preflight, and a verified 42/42 host-gate binding.
It is not promotable until replay passes. Do not rebuild it merely to add
MiniNDN, Mininet, Open vSwitch, NLSR, or the replay fixture; those belong to the
host substrate. After the separate host-substrate preflight and host-
orchestrated replay pass, expected: `PROMOTABLE`, one SIF digest, Python 3.10 native import, closed
RPATH/`ldd`, Boost 1.71 and NDN-SVS `Experimental`, real workload `--help`,
CPU/CUDA ORT inventory, `tokenizers` present, and deployed PyTorch/Transformers
absent. Build the Python extension inside the candidate SIF/sealed builder only.
The candidate must also embed the exact sealed framework/application source and
workload used by the gates. Do not qualify a SIF by bind-mounting a newer
checkout, `_ndnsf.so`, Python package, or workload over it.

Before starting the 30-entry replay, run one bounded M01 launcher smoke with
the exact candidate. Inspect the generated commands and require Apptainer
1.5.3 `--home <node-home>:<node-home>` mappings (not `--env HOME=...`),
distinct HOME/PIB paths, successful Controller-to-repository bootstrap
decryption, and a non-empty permission snapshot before the request. A smoke
failure is retained as negative launcher/identity evidence and invalidates the
sealed candidate after any runner fix; do not classify it as a model or ONNX
failure and do not continue to the full replay.

Replay the same M01-M14 manifest by keeping MiniNDN/Mininet/OVS/NLSR on the
host and launching each node's NFD, Controller, repository, Provider, User, and
ORT process from that exact SIF with `apptainer exec --cleanenv`. This is a
final packaging/reproducibility check, not the primary debugging loop. First
classify a mismatch as host-substrate, container launch/mount/environment, or
SIF-runtime failure; do not automatically rebuild the SIF.

## 7. G4 host MiniNDN replay with exact-SIF applications

```bash
sudo -E env \
  SPEC175_RUN_REAL_MININDN=1 \
  SPEC175_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer \
  SPEC175_RUNTIME_SIF="$(jq -er .sif.path "$SPEC175_CANDIDATE_MANIFEST")" \
  SPEC175_CANDIDATE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
  python3 packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --source-seal "$SPEC175_SOURCE_SEAL" \
  --sif "$SPEC175_SIF" --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --sif-sha256 "$(sha256sum "$SPEC175_SIF" | awk '{print "sha256:"$1}')" \
  --output results/spec175/g4/qualification-manifest-v1.json
```

Until T023 implements and tests this tracked host driver, no existing pytest or
dry-run may be reported as G4. The driver must create the host MiniNDN topology,
invoke the exact SIF for every NFD/application process, and emit a 30-entry G4
manifest. It must never start MiniNDN inside the SIF. Expected:
M01-M14 each pass in three clean processes (42/42), exact tiny-model
tokens/final results, real controller/NFD/SVS/role processes, complete Interest/
Data lineage, ACK-driven mapping change under M10, and clean process-tree
teardown. Stop here on any failure.

## 8. Review promotion manifest

Before Tiger, verify the candidate rather than selecting values interactively:

```bash
jq '{status,git,sif,model,tokenizer,workload,lowerGates}' \
  "$SPEC175_CANDIDATE_MANIFEST"
sha256sum "$(jq -er .sif.path "$SPEC175_CANDIDATE_MANIFEST")"
```

Required: exact hash equals the manifest and every lower gate is `PASS` or
`PROMOTABLE` as specified. A dirty candidate must include an explicit source
seal and scoped diff; unrelated dirty files are not copied.

Bind the positional Tiger interface once; do not reconstruct these values per
gate:

```bash
export SPEC175_SIF_SHA256="sha256:$(sha256sum "$SPEC175_SIF" | awk '{print $1}')"
export SPEC175_WORKLOAD="$PWD/packaging/ndnsf-di-container/jobs/spec175/workload.json"
export SPEC175_REMOTE_SIF="/project/$USER/ndnsf-di/releases/<candidate>/runtime.sif"
export SPEC175_PRE_TIGER_CHECKLIST="$PWD/results/spec175/tiger/<candidate>/pre-tiger-checklist.json"
export SPEC175_PRE_TIGER_CHECKLIST_VALIDATION="$PWD/results/spec175/tiger/<candidate>/pre-tiger-checklist-validation.json"
export MODEL_MANIFEST="$PWD/<frozen-model-manifest.json>"
export REMOTE_MODEL_ROOT="/project/$USER/ndnsf-di/models/<model-digest>"
```

`MODEL_MANIFEST` and `REMOTE_MODEL_ROOT` are ignored by the planned `control`
gate and mandatory for G5--G7.

## 9. G4T Tiger current-SIF deployment control

Before model staging, compare the rendered launcher with the proven Spec170
D0/r23 route. The machine-readable delta record must cover Apptainer 1.5.3,
Python ABI, SIF and bundle hashes, `cd "$BUNDLE"`, isolated HOME/PIB/TPM for
NFD/controller/User/each Provider, provider count, timeouts, node layout, child
exit propagation, and the explicit absence of MiniNDN/NLSR/SIF construction on
Tiger. An unlisted delta refuses submission.

T024 must add and mutation-test a `control` gate before the source freeze.
T025 executes this command only after T023 produces the exact G4 candidate:

```bash
CLOSURE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
SIF="$SPEC175_SIF" SIF_SHA256="$SPEC175_SIF_SHA256" \
WORKLOAD="$SPEC175_WORKLOAD" REMOTE_SIF="$SPEC175_REMOTE_SIF" \
REMOTE_SIF_SHA256="$SPEC175_SIF_SHA256" \
PRE_TIGER_CHECKLIST="$SPEC175_PRE_TIGER_CHECKLIST" \
PRE_TIGER_CHECKLIST_VALIDATION="$SPEC175_PRE_TIGER_CHECKLIST_VALIDATION" \
packaging/ndnsf-di-container/jobs/spec175/submit.sh control
```

T024 must implement the tracked checklist validator and wire it into
`submit.sh` before this command is usable. A failing checklist must stop before
SSH, upload, remote mutation, or allocation; a passing checklist does not
replace the repository-specific preflights or the real control lifecycle.

Expected: one bounded CPU/no-GPU Slurm job stages and verifies the exact current
SIF, starts real NFD plus one Controller, one User, and four Providers, observes
Request -> four ACKs -> provider-specific Selection -> final Response, checks
every child exit, and ends `COMPLETED 0:0`. This is a deployment/configuration
control only; it cannot satisfy G5, G6, or G7. Stop before model staging if it
fails.

## 10. G5 Tiger Qwen3.6-27B stage/cache-readiness gate

**Planned**:

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh stage-readiness
```

Expected: the exact SIF loads and executes each adapter-certified stateful 27B
ONNX stage with CUDA ORT, verifies prefill plus one incremental decode transition
and its complete state-component continuity, and stages the immutable artifacts
once into a hash-verified node-local cache. It must retain state in persistent
CUDA I/O bindings, show no complete-state host round trip after prefill, and run
the registered eight-token paired control: one excluded warmup plus three
alternating cached/full-prefix pairs per stage, exact parity, and lower median
cached model-compute time. The existing full-context-only reference chain cannot
satisfy G5. Record staging/load/execute/transfer/control timing separately; G5
has no end-to-end streamed-generation claim. A frozen Qwen3-0.6B CUDA
or CPU run may be kept as an optional diagnostic for adapter/control-flow
problems, but cannot satisfy G5, G6, or G7. The 0.6B subject is deliberately
not the final subject: it does not establish the 27B state schema, memory
pressure, GPU residency, or three-Provider qualification.

## 11. G6 Tiger three-Provider functional gate

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh multi-provider
```

Required primary result: pinned Qwen3.6-27B three-role split `[0,21)`,
`[21,42)`, `[42,64)`, one GPU/Provider/role, six measured exact invocations,
one plan/prefill each, complete activation/feedback/event lineage, active ONNX
Runtime `CUDAExecutionProvider` for model computation. ORT may use the CPU EP
only for bounded int64/bool shape-control nodes; any CPU model-compute node is
a failed deployment. Every non-prefill epoch must consume the new token plus the
exact Qwen3.6 decode-state bundle; full-context recomputation per token and
runtime source overlays fail this gate. Every Provider must automatically reuse
its local state through the production role session; harness-managed feedback,
state carried over NDN, unexpected misses, or complete-state host round trips
also fail.

## 12. G6C Tiger conversation-residency functional control

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh conversation-residency
```

Expected: the same candidate and three-role Qwen3.6-27B placement complete one
two-turn conversation. Turn 1 first proves ordinary request-local decode state,
then state-only terminal-prefix finalization when needed, and atomic all-role
promotion into the parent conversation checkpoint. Turn 2 has a fresh
Request/generation, submits only the new application input plus the opaque parent
checkpoint, derives and prefills only the appended canonical suffix, restores all three
role-local states through real `GPU_RESIDENT -> HOST_RESIDENT -> PREFETCHING ->
GPU_RESIDENT` transitions, and exactly matches the full-transcript CUDA oracle.
The manifest records state bytes/transfer/prefetch latency and proves that model
weights did not move and no state tensor crossed NDN. One unavailable-role run
must fail explicitly without fallback; the paired authorized full-context
fallback must perform one recorded full prefill and return the exact result.
This control is functional and supplies no performance sample.

## 13. G7 Tiger performance qualification

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh performance
```

Expected: three independent processes, one excluded cold plus ten warm
invocations each, 30 warm units total. G7 inherits the passing G5 paired cache-
effectiveness evidence and reports state residence/transfer counters with the
component timings. The summary assigns exactly one verdict:

```text
PERFORMANCE_PASS
FUNCTIONAL_PASS_PERFORMANCE_MISS
FAIL
```

`PERFORMANCE_PASS` requires exact correctness, zero fallback, median warm
steady-state >=20.0 token/s, and p95 inter-token <=75 ms.

## 14. Completion check

The feature is implemented only when:

```text
G0 PASS -> G1 PASS -> G2 PASS -> G3 host/CPU MiniNDN PASS ->
G4 final SIF/replay PASS -> G4T current-SIF Tiger control PASS ->
G5 PASS -> G6 PASS -> G6C PASS -> G7 verdict recorded
```

`implemented`, `compiled`, `one unit suite passed`, `SIF built`, `model loaded`,
or `GPU used` are intermediate evidence levels, not completion.
