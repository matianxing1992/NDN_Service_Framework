# TigerCluster Experiment Admission Contract

## Immutable inputs

Before submission, freeze and hash:

- git source commit/tree and uncommitted-patch status;
- SIF path/digest and foundation identity;
- model, tokenizer, graph, shard and manifest identities;
- distinct `localFixtureManifestDigest`, `remoteSmallStageManifestDigest`, and
  `remoteLargeStageManifestDigest` bindings; a local tiny-Qwen result cannot
  satisfy a remote-model gate, and a remote Qwen3-0.6B manifest cannot force
  the 8 GiB host to load that model;
- the complete runtime source/package-data bundle digest, the admitted Gate B
  manifest digest, and the exact-SIF/CUDA Gate C result digest as separate
  bindings; a partial overlay or one gate result cannot satisfy another;
- placement strategy code/configuration;
- prompt set and generation configuration;
- NDN topology, route and strategy configuration;
- security policies/certificates by non-secret identity;
- Slurm script, node/GPU constraints and environment;
- the immutable resource profile (partition, node count, GPU type/count,
  memory-per-node, CPU count, and time limit);
- analyzer and expected schedule.

No secret material is copied into evidence.

## Admission gates

| Gate | Required outcome |
|---|---|
| A - focused | lifecycle, security, cache, progress, range and dataflow regressions pass |
| B - real MiniNDN | independent User/Controller/Repo/three Providers, real NFD routing and full response using the frozen tiny-Qwen three-role fixture; on the 8 GiB host enforce 6 GiB memory, 7 GiB memory-plus-swap, and zero cgroup OOM events; explicit `CPU_LOGIC` proves no GPU claim; a >7 KiB combined assignment case must reach all three Providers through bounded provider-specific Selection projections |
| C - exact container | the same source/SIF, V2 contract, topology and tiny-Qwen logic profile pass locally within the Gate B cgroup limits; an exact-SIF `CUDA` preflight with the remote model identity runs on one TigerCluster node and is required before Gate E |
| D - candidate audit | hashes, schedule, routes, GPU/storage capacity and evidence writer valid |
| E - remote small single | one complete authenticated multi-token response, zero CPU fallback |
| F - remote small repeated | 5 prompts x (1 warmup + 5 measured), complete schedule |
| G - remote large single | model larger than one GPU completes one full response |

Failure at any gate stops progression. When the development host lacks CUDA or
Apptainer, Gate C's CUDA portion runs as a bounded single-node TigerCluster
preflight before any three-node campaign; it is not Gate E and cannot authorize
a distributed-inference claim. Repair re-enters at Gate A with a new source
identity where code changed.

If a formal large-model row passes admission and then fails before inference
because the Slurm allocation's memory cgroup is independently shown to be the
limiting boundary, the failed identity remains closed and immutable. One, and
only one, replacement may be admitted with a new source/campaign identity and
an explicitly larger/frozen resource profile. A second resource replacement is
forbidden; if the replacement fails, Gate G remains unmet.

## Remote execution rules

- GPU compute and Provider processes run only inside Slurm allocations.
- Full Qwen3-0.6B or large-model shard materialization/loading and every
  high-memory, CUDA-capacity, or unknown-peak workload run only inside a
  TigerCluster Slurm allocation. The 8 GiB host is limited to the bounded
  tiny-Qwen profile or non-materializing metadata/state-machine tests.
- Reuse pre-existing qualified SIF/model/repository assets when identities match.
- Do not insert a Provider-settle sleep. The User sends the Request after normal
  process/route/bootstrap readiness checks; request-driven ACK/planning then owns
  preparation and execution.
- Do not repair routes, restart Providers, mutate timeouts, or replace payloads
  inside a formal identity.
- Progress may extend a no-progress deadline only under the lifecycle contract;
  the hard experiment bound remains fixed.
- First logic/security/analyzer failure closes the row. A pre-inference
  resource-cgroup failure follows the one-new-identity exception above;
  preserve the original partial stage and Repo evidence in all cases.

## Small-model schedule

1. One deterministic Qwen3-0.6B single-request control.
2. If complete, five pinned real prompts.
3. For each prompt: one warmup followed by five measured invocations.
4. Keep the same allocation and compatible Provider residency unless an
   environmental fault forces the campaign to close.
5. Preserve every answer, TTFT, inter-token latencies, total latency, tokens/s,
   phase timings, cache evidence, transferred bytes, load events and failures.

## Large-model schedule

Only after the complete small-model schedule, run one deterministic request for
the pinned larger Qwen model using the same API and lifecycle. Do not schedule a
large-model repetition campaign until this one response completes.

## Acceptance and claims

- Correctness requires the full authenticated Response, not Provider startup,
  catalog ACTIVE, bytes fetched, READY, or one stage.
- Warm reuse requires matching cache/load/transfer evidence, not a shorter job.
- Repository throughput uses repository counters/time intervals, not Slurm wall
  time or model preparation duration.
- Results describe the pinned deployment and workload. General performance or
  model-quality claims require a separate powered experiment.
