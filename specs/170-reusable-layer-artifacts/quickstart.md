# Quickstart: Spec 170 Validation Ladder

**Status**: Planned validation guide. Commands naming Spec 170 test files become
runnable only after their implementation tasks create those entry points.

## Preconditions

- Active feature points to `specs/170-reusable-layer-artifacts`.
- Source, model, adapter, canonical artifact profile, security policy, topology,
  and container identities are recorded before a formal run.
- No TigerCluster job is submitted until local contract, MiniNDN, and exact-SIF
  gates pass.
- Candidate freeze occurs only after Gate A, Gate B, Gate C, every executable/
  security/build/harness change, and model/artifact preparation are complete.
  After freeze, commands may execute the bound hashes and write evidence only.
- MiniNDN uses the default wired `Minindn` topology unless a test explicitly
  studies wireless behavior; host NFD is diagnosis only, not final evidence.
- Performance windows are at least 60 seconds after readiness/warmup. A smoke
  mode stays under 10 seconds, emits `SMOKE_OK`, and supports no performance claim.

## Gate A - Contract and Property Tests

Planned focused entry points:

```bash
python3 -m pytest -q \
  tests/python/test_spec170_canonical_artifacts.py \
  tests/python/test_spec170_runtime_topology.py \
  tests/python/test_spec170_placement_v3.py \
  tests/python/test_spec170_hybrid_execution.py \
  tests/python/test_spec170_v2_v3_compatibility.py \
  tests/python/test_spec170_ack_no_reservation.py \
  tests/python/test_spec170_artifact_security.py \
  tests/python/test_spec170_content_addressed_reuse.py \
  tests/python/test_spec170_multi_device_provider.py
```

Expected outcomes:

- all valid degree vectors and rank/tensor maps seal deterministically;
- every invalid mutation fails at the intended contract boundary;
- `[1,2,1]` has four total ranks, one collective group, and two explicit
  degree-changing boundaries;
- an `M_i=1` stage has no tensor collective or phantom rank;
- a sharded stage may contain sharded, replicated, owner-only, and locally
  derived tensors according to its adapter recipe;
- a custom strategy receives only sanitized planning views; invalid proposal,
  timeout, exception, cancellation, or candidate-budget exhaustion publishes no
  Selection and is classified at the planning boundary;
- `2 x 12 GiB` cannot satisfy one unsplittable 20-GiB device requirement;
- V2 and V3 offers/assignments/cache identities do not cross-decode or cross-hit.

### In-process integration layer (before MiniNDN)

The unit/property suites should be followed by the L1/L2 integrated harness in
[`contracts/in-process-integration-tests-v1.md`](contracts/in-process-integration-tests-v1.md).
It uses real NDNSF packet handlers over ndn-cxx `DummyClientFace` and real
ndn-svs `SVSPubSub`/`SVSync` nodes with a bounded event loop. Run lifecycle,
canonical-reuse, `NDNSF_DATA_V1`, and protected-artifact cases before Gate B:

Each process creates one `NdnsfIntegrationEnvironment`, runs its measured
`bootstrap()` lane once, and starts request timing only after the shared
`READY` snapshot. Every case calls `beginRequest()` and
`markRequestPublished()` before exercising request flow, then calls
`resetRequest()` and proves request-local state is gone. See
[`contracts/in-process-environment-v1.md`](contracts/in-process-environment-v1.md).

```bash
./waf build --targets=integration-tests
build/integration-tests --run_test='NdnSvsSmoke/*'
build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/*'
```

The current target contains the SVS/NDNSF packet cases and native DI core cases;
`tests/python/test_spec170_integrated_flows.py` adds V3 lifecycle, canonical
assembly/reuse, `NDNSF_DATA_V1`, protected artifacts, and multi-device cases.
Together they catch most packet, TLV, token, replay, repair, deadline,
artifact, and partial-output wiring defects without NFD or MiniNDN. They cannot
prove routing, process isolation, deployment security, or GPU behavior, so they
supplement and do not replace Gate B.

## Gate B - Real MiniNDN

The implementation supplies one wired MiniNDN harness using `getPopen`, one log
per node, real Controller/Requester/three independent Providers/Repo, security
enabled, and the minimal real model.

Smoke:

```bash
sudo -E python3 <spec170-minindn-runner> \
  --output-dir results/spec170/smoke \
  --auto-stop-seconds 8 \
  --smoke
```

Acceptance smoke requirements:

- marker `SMOKE_OK`;
- all processes terminate before `ndn.stop()`;
- request, ACK_CLOSED, Selection, Provider preparation, dependency data, and
  complete Response are observed;
- no performance interpretation.

Measured correctness/performance profile:

```bash
sudo -E python3 <spec170-minindn-runner> \
  --output-dir results/spec170/minindn-60s \
  --auto-stop-seconds 70 \
  --measurement-seconds 60
```

Harness rules:

- wait for a Spec 170 readiness marker before the measured window;
- discard routing/process warmup before measurement;
- set `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` to a bounded sampling rate;
- redirect each node's stdout/stderr to one node-specific log;
- keep topology, input, source, duration, and all controls equal for matched
  cold/warm or degree-vector comparisons;
- do not change SVS sync/suppression timings as part of this feature.

Required profiles:

1. CPU-allowed no-GPU complete response;
2. GPU-required no-GPU rejection without fallback;
3. simulated one-device V3 plus explicit V2 compatibility;
4. simulated two-device independent role bindings;
5. deterministic Provider-local and cross-Provider multi-rank collective
   emulation with distinct bundle/rendezvous evidence;
6. `[1,1,1]`, `[2,2,2]`, `[1,2,1]`, and `[2,1,2]` oracle comparison;
7. cold canonical fetch/assembly followed by exact warm reuse.
8. three concurrent normal-default V3 invocations across the three Provider
   processes, with no ACK reservation and independent queue/admission evidence.
   The fixed 8-GiB-host profile is one three-stage CPU pipeline in which each
   Provider owns one fragment and all three requests select the same three
   Providers, proving fragment single-flight/reuse without three full-model loads.

MiniNDN proves real network/security/lifecycle behavior. Simulated devices and
collectives are labelled control-logic evidence, not CUDA/NCCL evidence.

### Current executable cache-reuse diagnostic

The following command is an executable local diagnostic while the complete V3
Provider-assembly harness is still under construction. It uses the cached
`Qwen/Qwen2.5-0.5B-Instruct`, never downloads or rebuilds a model, and keeps
stage bytes in a persistent content-addressed store. Each run directory then
contains only manifest files and relative links:

```bash
sudo -n timeout 240s env HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
  python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-transformers \
  --qwen-model Qwen/Qwen2.5-0.5B-Instruct --qwen-revision main \
  --qwen-content-store results/.ndnsf-di-content-addressed \
  --static-routing-only --nlsr-wait-s 0 \
  --warmup-requests 1 --measured-requests 3 --max-new-tokens 2 \
  --test-only-allow-ephemeral-app-state \
  --output-dir /tmp/spec170-minindn-qwen-multi \
  --prompt 'What is NDN?'
```

This diagnostic passed on 2026-08-04 (three measured responses, p50
416.57 ms; three content-addressed stage objects). It is recorded in
`evidence/content-addressed-minindn-20260804.md`; it does **not** close T026
because the normal-default V3 Selection/Provider-assembly and the required
three cold/warm statistical blocks are still separate acceptance conditions.

## Gate C - Exact OCI/SIF Parity

Planned container tests:

```bash
python3 -m pytest -q \
  tests/container/unit/test_spec170_allocation_topology.py \
  tests/container/unit/test_spec170_exact_sif_gate.py
```

Then run the exact candidate SIF in:

- CPU-only mode with no GPU exposure;
- one-GPU CUDA preflight;
- two-GPU visibility/topology preflight when a qualified allocation is available.

The container must use the same installed code/contracts as Gate B and must not
inject test-only role, device, shard, security, or fallback defaults.

## Freeze Cut - Required Between Gate C and Gate D

After Gate A, Gate B, and Gate C pass, create T029's frozen-candidate manifest.
It binds source, OCI/SIF, dependencies, native/Python installed files, every job
file, model/canonical payload, prompt corpus, security, routes, schedule, and the
three local gate reports. Run the mismatch self-test before submission.

From this point onward, do not rebuild, reprepare, edit a job/harness, change a
configuration/workload, or patch source under the same candidate ID. A mismatch
is `INVALID_CANDIDATE`; fix it before freeze and create a new candidate.

## Gate D - TigerCluster 0/1/2 GPU Visibility

Create three immutable GPU-count classes from the same source/SIF/config bundle.
D0 and D1 each use their own job identity. The two-GPU class contains at least
the distinct D2a and D2b topology/job identities; counting GPU classes MUST NOT
collapse those identities.

### D0: zero GPU

- do not request a GRES/GPU;
- omit GPU exposure at Apptainer launch;
- require `acceleratorDevices=[]` in the signed offer;
- execute a CPU-allowed bounded profile or verify GPU-required rejection.

### D1: one GPU

- request exactly one GPU;
- launch the SIF with `apptainer exec --nv`;
- require scheduler allocation, runtime probe, signed offer, Selection, and
  loaded-runtime identity to name the same one device;
- require one complete minimal-model CUDA response and `CPU fallback = 0`.

### D2a: two GPUs for one Provider

- request exactly two GPUs for one task/Provider process;
- launch with `--nv`;
- require both and only allocated devices in probe/offer/Selection;
- first execute two independent single-device roles;
- then execute one Provider-local two-rank role supported by the frozen
  candidate.

Two single-GPU Providers do not satisfy D2a. If the allocation cannot expose two
GPUs to one Provider process, record `BLOCK` rather than changing the claim.

### D2b: one tensor group across two Providers

- use a separately declared two-GPU topology and launch two Provider runtimes,
  each restricted to one allocated GPU;
- require two Provider-local offers/bundles and no cross-offer device binding;
- execute one authenticated two-rank logical role across the Providers;
- retain rendezvous, peer, group/epoch, local admission, transport, and complete
  output/failure evidence.

D2a and D2b are separate claims. Neither may be substituted for the other.

### D2h: heterogeneous hybrid qualification

- after D2a and D2b close, run the `[1,2,1]` and `[2,1,2]` profiles on a
  fixed two-Provider/two-GPU placement using the same frozen candidate;
- require `[1,2,1]` mapping `P0/G0={S0R0,S1R0}` and
  `P1/G1={S1R1,S2R0}`, and `[2,1,2]` mapping
  `P0/G0={S0R0,S1R0,S2R0}` and `P1/G1={S0R1,S2R1}`;
- treat all ranks co-resident on one GPU as one plan-local `EXCLUSIVE_PLAN`
  admission vector with summed phase peaks, not as MPS/multi-tenant sharing;
- require separate rank, collective, redistribution, data-driven activation,
  oracle, and failure evidence rather than combining D2a and D2b summaries;
- reuse an unchanged accepted allocation only when its topology and resource
  envelopes satisfy the sealed hybrid plan; otherwise use a new immutable job
  identity;
- if the two-GPU resource/topology envelope cannot support the profile, record
  the hardware hybrid claim as `BLOCK`; MiniNDN emulation, D2a, or D2b cannot
  substitute for it.

## Repeated Workload

For every accepted measured configuration, use primary prompt IDs `P01-P05` and
exactly three clean-start blocks for publication-quality evidence. Within each
start and prompt: reset the tested model/profile caches and record the inventory,
run one measured cold request, run one unmeasured warmup, then run five measured
warm requests. This yields 15 cold and 75 warm rows per configuration. Fewer than
three complete blocks is `EXPLORATORY` and cannot close SC-034. Prompt IDs
`P06-P08` are correctness-only and do not enter performance aggregates.

Retain full answers, request/attempt/plan/rank identities, TTFT, per-token and
total latency, tokens/s, Repo bytes, assembly/load events, cache state, device
mapping, per-device resources, progress, and failure rows.

Report cold and warm distributions separately using the predeclared hierarchical
10,000-iteration bootstrap, paired warm-latency ratio/effect size, equivalence
margin, Holm family, and exact failure intervals in `experiment-plan.md`. Do not
delete negative rows or infer a large-model result from the minimal-model gate.

## Fault Profiles

At minimum run:

- stale profile/snapshot/resource sequence;
- missing/unoffered/lost device;
- partial device-set availability;
- weights fit but transient/KV/collective envelope does not;
- missing/duplicate/orphan rank;
- wrong tensor distribution/layout/collective order;
- omitted or wrong `1<->2` redistribution;
- delayed rank/downstream early-start attempt;
- corrupt canonical object/manifest and false loaded-runtime hit;
- Repo progress/no-progress deadline and request/attempt replay mismatch.

Each result must contain the expected narrow lifecycle class and last verified
progress checkpoint.

## Acceptance Summary

The feature is not ready merely because a GPU probe or one token succeeds. It is
ready only when:

- deterministic contract and negative suites pass;
- real MiniNDN completes authenticated multi-token outputs;
- exact-SIF parity passes;
- TigerCluster 0/1/2 allocation-to-offer-to-Selection mapping is exact;
- independent and multi-rank device profiles pass their respective gates;
- heterogeneous `[1,2,1]` and `[2,1,2]` full outputs match the frozen oracle;
- exact warm reuse and all failure/observability invariants hold.
