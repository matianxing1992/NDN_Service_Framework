# T012 bounded local Docker acceptance

## Accepted run

The canonical T012 run is:

```text
results/spec163-local-docker-20260729_015529
```

It was produced with:

```bash
tests/container/placement-preparation/run.sh
```

`gate-summary.txt` ends in `SPEC163_LOCAL_DOCKER_ALL_GATES_PASS`.
`docker-inspect.txt` records exit code 0, no OOM kill, 4 GiB memory, 5 GiB
memory-plus-swap, and a 1024 PID limit. The container ran one private NFD, one
Controller, one requester, and three Providers. `resource-boundary.txt`
records zero Torch/Qwen processes before and after the run, one NFD, fake
byte-sized payloads, and `model_loaded=false`.

## Lifecycle ordering evidence

All timestamps below are monotonic nanoseconds from `fake-di-lifecycle.log`.

| Event | Timestamp |
|---|---:|
| Final plan commit begins | 1785308173538600874 |
| Stage 0 starts | 1785308173550724120 |
| Stage 0 publishes fan-out object | 1785308173550817111 |
| left role receives input | 1785308173555997203 |
| right role receives input | 1785308173555973196 |
| right role finishes local preparation | 1785308174549791269 |
| right role starts | 1785308174549850170 |
| merge receives both inputs | 1785308174554934895 |
| merge starts | 1785308174554972475 |
| secure Response accepted | 1785308174561454321 |

The final Selection/plan commit precedes complete local readiness by
1011.190 ms. Stage 0 starts and publishes while the right role is still
preparing, respectively 999.067 ms and 998.974 ms before that role becomes
ready. The right role demonstrates input-before-model: input arrives
993.818 ms before readiness, and execution starts 0.059 ms after readiness.
The left role demonstrates model-before-input: readiness precedes input by
6.821 ms. The merge role starts 0.038 ms after its second required input.
Thus execution is gated by the later of local readiness and dependency input,
not a global preparation barrier.

## Contract and security gates

The retained run passes:

- three-Provider selective ACK collection and exact selected execution;
- normal HELLO authorization, encrypted permission delivery, plaintext
  permission rejection, and provider permission enforcement;
- REQUEST/SELECTION `/SERVICE/<service>` and ACK/RESPONSE
  `/PERMISSION/<service>` NAC-ABE routing;
- UserToken and ProviderToken binding, mismatch rejection, replay rejection,
  and opaque Selection transaction replay;
- one crash-atomic multi-role assignment tuple and byte-identical acceptance
  replay;
- the secure deferred
  `begin_collaboration -> ACK_CLOSED -> commit_plan -> Selection -> acceptance
  -> Response` round;
- signed offer/assignment envelope and aggregate GPU-capacity checks,
  concurrent capacity exclusion, dependency DAG fan-out/fan-in, object
  manifests, result contracts, cancellation/Response fencing, adoption, and
  compensation;
- static Core ownership and non-DI Core fixtures;
- default `DEFERRED`, fixed-plan `PREPLANNED`, idempotent plan retry,
  conflicting/early/late commit rejection, and non-blocking planning;
- LLM, object-detection, and opaque-container adapters using the same public
  request/carrier, plus model/cache/derived-state contract tests.

## Bounded history corpus

`test_ndnsf_di_lifecycle_history.log` records 36 exhaustive bounded event
histories and concurrent seeds 163000 through 163031. The generated schedule
categories are duplicate, reorder, loss, retry, partition, restart, and
concurrency. Counterexamples, if found, are retained by seed with a shrunk
event trace. This corpus observed zero violations of:

1. at-most-once admission per role generation;
2. one accepted terminal result;
3. resource exclusion;
4. authority fencing;
5. bounded cleanup.

This is a bounded state/history result, not a proof for arbitrary executions.

## Scope boundary

This run validates the real NDNSF security and collaboration carrier with a
minimal fake DI workload. It does not load a model, make a performance claim,
prove malicious-computation correctness, prove distributed atomicity or
deadlock freedom, or validate a universal optimizer. Earlier runs are
diagnostic/superseded; only the accepted path above is the T012 closure
artifact.
