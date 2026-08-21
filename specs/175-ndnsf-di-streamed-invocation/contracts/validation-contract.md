# Validation Contract: Spec 175 Promotion Gates

## Global rules

- Gates run in order G0 through G7. Any mandatory failure blocks every later
  gate; no `--force` path exists for evidence acceptance.
- Every process uses fixed workload seed `1750001` and Greedy sampling for exact
  token comparison. Fault scheduling uses fixed seed `1750002`.
- Every gate writes `qualification-manifest-v1.json` and a SHA-256 evidence
  index. Logs are bounded and exclude plaintext prompt/answer/KV/logits.
- Required tests may not be skipped. A missing dependency is a failed gate, not
  a skip/pass.
- Healthy deterministic cases run in three fresh OS processes. Fault cases run
  once per registered fault per fresh process unless the case table says three.
- The first full-model Tiger invocation is cold preparation and is recorded but
  excluded from warm latency distributions. Three fresh warm processes are
  mandatory.
- No runtime parameter may change between registered repetitions. A changed
  parameter creates a new campaign ID and cannot be pooled with the old one.

## Deterministic CPU fixture

Path:

```text
tests/fixtures/spec175/tiny-causal-lm-v1/
```

The committed generator creates canonical content-addressed artifacts, not a
Python fake runner:

- vocabulary size 32;
- hidden size 8;
- four transformer-like deterministic role blocks with explicit KV input/output;
- combined one-role ONNX graph and graph-valid 2-role/4-role partitions;
- WordLevel `tokenizer.json` with EOS token ID 2;
- normal prompt token IDs `[11,12,13]` and greedy expected generated IDs
  `[4,5,6,7,8,9,10,2]`;
- early-stop prompt with expected IDs `[14,15,2]`;
- maximum-token prompt with no EOS before 8 tokens;
- manifest containing every graph/initializer/tokenizer/oracle digest.

`tests/fixtures/spec175/build_tiny_causal_onnx.py` must regenerate byte-identical
artifacts under the locked ONNX version or fail with a manifest difference. Test
runners consume the manifest and may not rewrite it.

## G0 - Static and contract gate

Required checks:

1. Spec Kit structure audit, no placeholder or unresolved clarification.
2. TLV collision scan proving `0xF636..0xF656` are unused before implementation
   and uniquely assigned afterward.
3. API/wire/default consistency scan across spec, plan, data model, contracts,
   C++ headers, Python options, and CLI manifests.
4. Source-owner map proving the new lifecycle extends current `ServiceUser`,
   `ServiceProvider`, `NDNSFMessages`, terminal guard, Qwen session, and exact
   collaboration primitives.
5. Runtime dependency scan proving deployed entry points do not import or link
   PyTorch/Transformers.

Planned command:

```bash
python3 scripts/spec175_contract_gate.py \
  --feature specs/175-ndnsf-di-streamed-invocation \
  --output results/spec175/g0/qualification-manifest-v1.json
```

Pass: every requirement is mapped, no collision/drift/placeholder exists, and
the manifest is `PASS`.

## G1 - Unit gate

### Native cases

`InvocationStreamMessage`:

- canonical options/event/End/completion round-trip;
- every missing/duplicate/out-of-order/unknown field;
- range, oversize, invalid enum, malformed digest, and noncanonical integer;
- exact Data name build/parse and every component mutation;
- binding/transcript/nonce deterministic test vectors;
- protected-grant length/domain tests, outer/plaintext grant rejection, and
  key-free log/evidence scan;
- End/Response match and mismatch matrix.

`InvocationStreamLifecycle`:

- all valid states and every invalid transition;
- cursor 1 start, no gaps at publisher, duplicate suppression, reorder drain;
- retry 0/1/3 and absolute deadline;
- callback/iterator single-consumer rule;
- publisher/callback/reorder capacity and backpressure timeout;
- cancellation races before admission, after admission, after End, and after
  Response;
- one terminal claim and no callback after terminal;
- Normal and Targeted bindings; unary allocates no stream state.

`DiQwenGenerationSession`:

- one prefill, incremental decode epoch convention, token/event cursor equality;
- EOS and stop success before maximum, exact maximum success, invalid early max;
- incremental Unicode and cross-token stop matching;
- every `KvStateIdentityV1` field mutated independently and rejected;
- transactional KV commit/rollback;
- replacement disabled, one replacement enabled, second replacement rejected;
- stale attempt/generation/event/feedback rejection;
- complete final payload/transcript consistency.

### Python cases

- frozen defaults/ranges and C++ parity;
- bytes and typed codec paths;
- async iterator/result/cancel;
- callback mode and double-consumer rejection;
- Python Provider writer binding, context variant, exception containment, and
  return-without-terminal failure;
- provider `publish_event`/`finish_stream` and duplicate terminal claim;
- no runtime Transformers/PyTorch import.

Commands:

```bash
./waf configure --with-tests
./waf build -j4
build/unit-tests --log_level=test_suite
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_streamed_invocation_api.py \
  tests/python/test_spec175_cpu_fixture.py \
  tests/python/test_spec175_evidence.py
```

Pass: complete `build/unit-tests` passes; required Python tests pass with zero
skip; unchanged unary, Targeted, security, StreamFacade, and Spec 174 focused
regressions named in the manifest pass.

## G2 - CPU in-process integration gate

Boost suites:

```text
Spec175InvocationStream
Spec175DiStreamedGeneration
```

Mandatory cases:

| ID | Providers/roles | Fault | Expected |
|---|---|---|---|
| I01 | 1/1 | none | 8 oracle tokens, EOS, one End/Response |
| I02 | 2/2 | none | same oracle; one prefill/plan |
| I03 | 4/4 | none | same oracle; exact activation lineage |
| I04 | 2/2 | event 3 delivered after 4 | callbacks remain 1..8 |
| I05 | 2/2 | event 4 duplicated | one callback for cursor 4 |
| I06 | 2/2 | first fetch of event 5 lost | one retry then exact success |
| I07 | 2/2 | event 5 never available | explicit `EVENT_TIMEOUT`, no success |
| I08 | 2/2 | cancel after event 3 | exactly 3 callbacks, no Response success |
| I09 | 2/2 | wrong signer/key/name/binding | reject before reorder insert |
| I10 | 2/2 | callback throws at event 3 | contained callback failure/cancel |
| I11 | 2/2 | queue capacity 1, slow consumer | bounded backpressure, no drop |
| I12 | 2/2 + spare | final Provider killed after event 3, replacement opt-in | new attempt recomputes; logical transcript exact |
| I13 | 2/2 + spare | same failure, replacement default | terminal failure, no re-execution |
| I14 | unary YOLO fixture | none | existing one complete Response, no stream state |

Run each healthy I01-I03 and I14 in three fresh processes; run I04-I13 once in
fresh processes. Command:

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I14 \
  --healthy-repeats 3 \
  --seed 1750001 \
  --output results/spec175/g2
```

Pass: all exact token/result/state/security oracles match; no leak/high-water
bound exceeds signed options; manifests enumerate all process exits.

## G3 - Local SIF build and native preflight

Build one SIF through the current local Apptainer entry point and a frozen
candidate manifest. No Docker build, remote materialization, host-built Python
extension, or host site-packages may enter the candidate.

The preflight, inside the exact SIF, must verify:

- SHA-256 source seal and SIF hash;
- Python 3.10 executable, SOABI, `EXT_SUFFIX`, and real `import ndnsf._ndnsf`;
- native extension/library hashes, RPATH, and complete `ldd` closure;
- Boost 1.71, ndn-cxx, NDN-SVS `Experimental`, NFD, MiniNDN, pybind11 ABI;
- ONNX Runtime CPU and CUDA provider inventory and the exact libraries;
- `tokenizers` available; PyTorch and Transformers absent from deployed runtime;
- Spec 175 API symbols and `--help` for the exact workload entry point;
- artifact/model/tokenizer/workload paths resolved from the verified bundle cwd;
- writable scratch/output paths, no secret/key leakage, and bounded free-space
  check inherited from the current native SIF preflight.

Planned commands:

```bash
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST"

packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --output results/spec175/g3/qualification-manifest-v1.json
```

Pass: manifest status `PROMOTABLE`; there is one and only one SIF digest.

## G4 - Exact-SIF real MiniNDN CPU gate

Topology:

```text
controller c ----+
repository repo --+
user u -----------+-- router a
provider p0 ------+
provider p1 ------+
provider p2 ------+
provider p3 ------+
```

Every access link is 100 Mbit/s, 10 ms one-way delay, queue 1000 packets, and
0% baseline loss. Every named node runs its own NFD; Providers are separate OS
processes. The controller distributes real permissions/ABE credentials. The
repository serves the content-addressed tiny ONNX bundle. Admission control is
disabled and explicitly recorded; no host NFD or Python fake transport is valid.

Mandatory cases, each in three clean MiniNDN processes:

| ID | Roles | Fault | Expected |
|---|---:|---|---|
| M01 | unary 1P | none | unchanged unary oracle |
| M02 | streamed 1P | none | 8 exact tokens/one End/Response |
| M03 | streamed 2P | none | exact result and two-role lineage |
| M04 | streamed 4P | none | exact result and four-role lineage |
| M05 | streamed 4P | first event-3 Data dropped | bounded exact retry and success |
| M06 | streamed 4P | event 2 delayed 200 ms after event 3 | ordered callbacks and success |
| M07 | streamed 4P | duplicate event 4 injected | duplicate suppressed |
| M08 | streamed 4P | cancel after event 3 | no later callback or success Response |
| M09 | streamed 4P | stale-attempt event injected | rejected; accepted stream succeeds |

The harness records Request -> ACK closure -> plan -> Selection -> artifact
fetch/assembly -> prefill -> every activation -> token feedback -> external
event -> End -> final Response, including exact Interest/Data names.

Command:

```bash
sudo -E env \
  SPEC175_RUN_REAL_MININDN=1 \
  SPEC175_CANDIDATE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
  PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_spec175_real_minindn_gate.py
```

Pass: 27/27 fresh case repetitions pass, process-tree cleanup succeeds, exact
oracle and transcript closure hold, all capacities remain bounded, and evidence
uses the G3 SIF digest.

## G5 - Tiger single-GPU functional gate

Model: `Qwen/Qwen3-0.6B`, immutable revision
`e6de91484c29aa9480d55605af694f39b081c455`, exported offline to canonical
prefill/decode ONNX and consumed by one Provider role through CUDA ORT.

Workload:

- two fixed prompts stored by digest, never printed in operational logs;
- Greedy sampling, maximum 64 new tokens;
- one cold invocation excluded from latency summary;
- three fresh warm Provider/User processes per prompt;
- exact local CUDA-ORT oracle tokens and final payload;
- stream options are the version-1 defaults except `maxEvents=65`.

Pass: 6/6 measured invocations complete, at least 8 tokens each unless EOS is
earlier in the sealed oracle, exact token/final digests match, CUDA provider is
active, CPU fallback is zero, and one End/Response closes each invocation.

## G6 - Tiger three-Provider Qwen3.6-27B functional gate

Model: `Qwen/Qwen3.6-27B`, immutable revision
`6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`.

Existing frozen split and roles are retained:

```text
Stage0 layers [0,21)  -> Provider P0
Stage1 layers [21,42) -> Provider P1
Stage2 layers [42,64) -> Provider P2
```

Each Provider uses one allocated GPU and one complete role. The plan is built
from the ACK capability snapshot and shared through Selection; no offline
Provider binding is accepted. Offline work only exports canonical ONNX
graph/weights/adapter metadata.

Workload and repetitions match G5. Pass requires 6/6 exact successful measured
invocations, one plan/prefill per generation, exact activation/feedback/event
lineage, zero CPU fallback, all three GPUs used by their roles, and no runtime
PyTorch/Transformers import.

## G7 - Performance qualification

G7 reuses the passing G6 candidate and workload without changes. For each warm
invocation report:

- TTFT;
- every inter-token interval and p50/p95/p99;
- steady-state tokens/s, excluding first token and terminal drain;
- total latency and exact success;
- per-role ORT, activation fetch, sampling, feedback, event publication/fetch,
  and queue wait time;
- retransmissions, gaps, duplicate suppression, KV hit/miss/recompute;
- GPU utilization/memory, CPU, RSS, network bytes, and CPU fallback.

Verdict:

```text
PERFORMANCE_PASS iff
  all G6 correctness checks pass AND
  median warm steady-state tokens/s >= 20.0 AND
  p95 warm inter-token interval <= 75 ms

otherwise, if correctness passes:
  FUNCTIONAL_PASS_PERFORMANCE_MISS
```

No valid warm run is discarded. Summaries report count, failure count, min,
median, mean, p95, maximum, and per-process values. Performance miss evidence
must attribute the largest measured component; it must not trigger unregistered
parameter tuning inside the same campaign.

## Tiger submission command contract

Only the following top-level interface is allowed:

```bash
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  --candidate-manifest "$SPEC175_CANDIDATE_MANIFEST" \
  --gate single-gpu|multi-provider|performance
```

The script must run login preflight, bounded compute-node preflight, SIF/config/
model hash verification, rendered Slurm validation, explicit bundle cwd, and
remote free-space check before `sbatch`. It uploads only missing content-addressed
artifacts and never rebuilds or substitutes the SIF. Any preflight mismatch stops
before GPU allocation.
