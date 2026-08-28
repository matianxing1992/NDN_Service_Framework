# T013 frozen MiniNDN and Qwen3 evidence

## Accepted artifact

```text
results/spec163-minindn-matrix-v2-20260729_022847
```

The accepted directory combines two bounded invocations so the real MiniNDN
network run is not repeated while CPU generation is being repaired or audited:

```bash
sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper:/usr/lib/python3/dist-packages" \
  LD_LIBRARY_PATH="$PWD/build:/usr/local/lib:/opt/onnxruntime/lib:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/opt/ndnsf/lib" \
  /usr/bin/python3 Experiments/NDNSF_DI_PlacementPreparation_Minindn.py \
  --output "$PWD/results/spec163-minindn-matrix-v2-20260729_022847" \
  --skip-generation

python3 Experiments/NDNSF_DI_PlacementPreparation_Minindn.py \
  --output results/spec163-minindn-matrix-v2-20260729_022847 \
  --skip-network
```

The terminal marker is
`SPEC163_MININDN_QWEN3_ALL_GATES_PASS`. `summary.json` reports `PASS`.
`matrix.json` uses schema `ndnsf-di-spec163-minindn-matrix-v2` and retains
59/59 passing rows, 23/23 passing gates, four true runtime assertions, and 63
row-specific references to test methods, sub-assertions, C++ cases, or runtime
roles. The real deferred Collaboration invocation used Memphis for the
Controller/requester and UCLA, Arizona, and WUSTL for Providers A, B, and C.

## Frozen ModelRef

| Field | Value |
|---|---|
| Name | `Qwen/Qwen3-0.6B` |
| Revision | `e6de91484c29aa9480d55605af694f39b081c455` |
| Content digest | `sha256:d750558221ce493e67dc11a2b9057cf123e30747ad68677edb6e428de109209f` |
| Semantics digest | `sha256:d8bec4e6f4b494d138f0dad64fb1c90a3544ddb8d4bd3ef066ba86b1a4f7313f` |
| Frozen bytes | 1,519,197,900 |

`model-manifest.json` contains the resolved size and SHA-256 of every snapshot
file. The semantics digest binds that content digest, prompt set, per-prompt
answer contracts, fixed system instruction, chat-template use,
`enable_thinking=false`, greedy decoding, and the 64-token ceiling.

## Complete-generation campaign

`generations.jsonl` retains five real prompts, one excluded warmup per prompt,
and five measured repetitions per prompt: 5 warmups and 25 measured records.
All 30 complete answers are retained. Every record:

- passes its prompt-specific semantic answer contract;
- terminates with a tokenizer special end token;
- matches its frozen greedy reference;
- uses at most 37 generated tokens, below the 64-token limit; and
- obtains the same split/provider plan from the real LLM adapter,
  `PreSplitFirstStrategy`, and automatic planning path as the manual baseline.

| Metric over 25 measured generations | Value |
|---|---:|
| Success | 25/25 (100%) |
| TTFT p50 | 21,674.109 ms |
| TTFT descriptive p95 | 24,792.491 ms |
| Total latency p50 | 97,924.833 ms |
| Total latency descriptive p95 | 146,077.941 ms |
| Tokens/s p50 | 0.248 |
| Tokens/s descriptive p95 | 6.545 |
| Frozen model load | 1,084.915 ms |
| Automatic plan p50 / p95 | 9.222 / 16.288 ms |
| Manual plan p50 / p95 | 0.011 / 0.013 ms |

Every raw record includes the complete answer, generated token IDs, TTFT,
every token-step latency, total latency, tokens/s, semantic-contract result,
reference comparison, and matched manual/automatic plan evidence. The model
ran on CPU because this host exposes no CUDA device. These latency values are
descriptive measurements of this constrained local run, not a performance
claim or a substitute for controlled GPU measurement.

## Placement and preparation observations

The MiniNDN carrier measured:

| Stage | Value |
|---|---:|
| Planning | 13.034 ms |
| Dynamic graph split/assignment assembly | 10.574 ms |
| Final Selection commit | 2.360 ms |
| Scope-key publication | 2.375 ms, 64 plaintext bytes |
| DistributedRepo real store/fetch smoke | 67.147 ms, 10 payload bytes |
| Activation publication | 0.060 ms, 13 payload bytes |
| Activation fetch waits | 4.602 ms, 4.356 ms |
| Frozen model disk-to-CPU-RAM load | 1,084.915 ms |

The byte-payload values validate lifecycle attribution and network ordering;
they are not model-shard transfer performance. The four runtime assertions
prove ACK-bound automatic plan construction, one Provider holding multiple
roles, NAC-ABE routing, and actual `DistributedRepoSmoke` store/fetch.

After the accepted campaign completed, the current implementation was
postflight-tested again because the final audit wired the trusted
`SplitMaterializer`/`DistributedArtifactPublisher` boundary into
`AutomaticPlanningCoordinator`. The 16 Python gate files all pass, including
the enforced `materialize -> publish -> commit` order, exact pre-split
catalog injection and resolution without rematerialization, and zero commit on materialization,
publication, digest/role mismatch, or expired deadline. The full Waf build and
the standalone real DistributedRepo smoke also pass.

## Superseded and rejected evidence

`results/spec163-minindn-qwen3-20260729_014203` is superseded. Its network and
generation observations remain useful, but its v1 matrix assigned the same
broad gate set to rows instead of retaining row-specific proof and did not
exercise the final trusted artifact-preparation ordering.

`results/spec163-minindn-qwen3-20260729_013015` remains rejected. Its arithmetic
prompt produced deterministic but truncated and incorrect 64-token answers,
while the old predicate checked only non-empty reference equality. The
negative directory is retained rather than hidden.

## GPU deferral and claim boundary

No CUDA device is available locally. GPU preparation, utilization, zero GPU
reload, and exact warm-GPU-reuse criteria are therefore
`DEFERRED_NO_LOCAL_CUDA`; CPU state is not presented as GPU evidence.

This evidence proves the MiniNDN security/collaboration carrier, bounded
protocol matrix, exact frozen model identity, prompt-specific complete Qwen3
CPU reference generation, real repository store/fetch, and the trusted
artifact-before-commit ordering. It does not prove distributed Qwen model
execution, GPU warm reuse, TigerCluster or large-model performance,
malicious-computation correctness, distributed atomicity, deadlock freedom,
starvation freedom, or universal placement optimality.
