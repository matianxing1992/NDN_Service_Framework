# Experiment Plan: Local DI Deployment Gate

## Research claim

The local NDNSF-DI deployment path can repeatedly generate multi-token Qwen3
answers through real MiniNDN and the candidate container while preserving
invocation identity and correctly distinguishing progress from stalls.

## Material passport

| Item | Frozen value |
|---|---|
| Model | `Qwen/Qwen3-0.6B` |
| Revision | `e6de91484c29aa9480d55605af694f39b081c455` |
| Acquisition | local-only; no implicit download |
| Network | real MiniNDN and NFD |
| Roles | User, Controller/security carrier, at least 3 Providers |
| Prompts | 2 fixed non-empty prompts |
| Warmup | 1 invocation per prompt |
| Measurement | 3 invocations per prompt |
| Token minimum | 8 retained generated-token events per measured invocation |
| Backend | explicit CPU or GPU; requested and actual placement retained |
| Randomness | fixed seed recorded in workload and result |

The run records source revision, dirty-tree state, exact command, dependency
versions, kernel, hardware profile, model manifest digest, and result paths.

## Experimental unit

One measured invocation is the unit for generation latency and throughput. A
run contains six measured invocations. Warmups are recorded but excluded from
measured distributions.

## Measurements

For every measured invocation retain:

- complete decoded answer;
- time to first token;
- ordered latency for every generated token;
- total latency;
- generated-token count;
- tokens per second;
- requested and actual backend, device placement, and fallback count;
- terminal status and failure reason;
- complete admitted/rejected lineage.

Summaries report count, minimum, median, mean, p95, and maximum without deleting
failed or incomplete units.

## Monitoring and stopping

The harness records process liveness and operation progress. A valid advancing
progress event renews only the idle deadline. The absolute deadline remains
fixed. The run stops on:

- normal completion;
- explicit failure or cancellation;
- `STALLED` after idle expiry;
- `HARD_TIMEOUT` at the absolute deadline;
- process exit, OOM, or evidence-contract failure.

Tests of deadline semantics use an injected clock. The real run uses monotonic
time and retains every deadline transition.

## Validity controls

- **Construct validity**: fake payload and startup checks are lower fidelity and
  cannot satisfy model execution.
- **Internal validity**: host and container consume the same workload digest;
  source/model/image identities are immutable.
- **External validity**: local CPU success is not a GPU performance claim.
  TigerCluster is a later explicitly authorized stage.
- **Reliability**: multiple prompts and repeated measured invocations expose
  warm-cache and request-state effects.

## Acceptance

Accept only if all Gate A-D mandatory records pass, both summaries agree, all
six measured invocations meet the token and evidence contract, all negative
lineage/deadline cases reject correctly, and no TigerCluster submission occurs.
