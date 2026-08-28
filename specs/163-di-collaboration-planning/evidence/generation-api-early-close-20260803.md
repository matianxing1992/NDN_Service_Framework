# Full-generation API and ACK coverage seam — 2026-08-03

## Decision

NDNSF-DI exposes one durable `GenerationRequest` for a complete generation:
the full input/prefill payload is sent once, ACK collection and post-ACK
planning happen once, and the adapter/provider returns one complete result.
Autoregressive token/KV exchange is internal data-plane work. A presentation
layer may render that already-computed result incrementally, but it must not
turn output tokens into additional NDNSF messages or collaborations. The
existing per-token Qwen harness remains diagnostic and is not the production
application contract.

The generic Core now accepts an optional application-owned
`CollaborationAckCoverageHandler`. It sees only candidates that passed the
normal ACK authentication, token, policy, and duplicate checks. A true result
marks the ACK window expired and produces the same immutable `ACK_CLOSED`
snapshot. It cannot select Providers, inspect a model graph, publish a split,
commit a plan, or alter the closure digest. `AckRoleCoveragePolicy` is the
NDNSF-DI helper that validates `DIProviderOfferV2` views and checks a bounded
role-coverage hint.

## Implemented surfaces

- `ServiceUser::BeginCollaboration(..., CollaborationAckCoverageHandler)`;
- Python `ServiceUser.begin_collaboration(...,
  ack_coverage_predicate=...)` and native pybind binding;
- `AckRoleCoveragePolicy` in `app_sdk.placement`;
- `AutomaticPlanningCoordinator(..., ack_coverage_roles=...)`;
- `GenerationRequest`, `AutomaticPlanningCoordinator.generate()`,
  `APPClient.generate()`, and the network facade forwarding one request;
- request envelope task metadata `generation_mode=FULL` for the complete
  generation surface;
- Qwen automatic-planning configuration supplies stage roles only as an early
  coverage hint; graph/split/assignment still occur after `ACK_CLOSED`.

## Verification

Local source checks passed:

```text
python3 tests/python/test_ndnsf_di_automatic_collaboration_plan.py  # 13 OK
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  LD_LIBRARY_PATH=$PWD/build \
  python3 tests/python/test_ndnsf_deferred_collaboration.py          # 4 OK
python3 -m py_compile <modified Python sources>                      # PASS
./waf build -j2 --targets=ndn-service-framework                       # PASS
python3 pythonWrapper/setup.py build_ext --inplace                    # PASS
```

The current-source real MiniNDN Qwen3-0.6B Gate B passed with reused
content-addressed artifacts:

```text
results/spec165-minindn-generation-api/20260803T033024Z-8a8d3d20
```

The default blocking Gate A–D aggregate also passed with the existing CPU
candidate image and the same reused preparation:

```text
results/spec165-generation-api-full-cpu/20260803T033254Z-9889127f
```

Both records report `tigerClusterSubmitted=false`. No model preparation,
foundation rebuild, or remote submission was performed for this change.

## Boundary before TigerCluster

The current-source real MiniNDN FULL gate now passes with the reused
content-addressed preparation:

```text
results/spec165-full-generation-minindn-v10/20260803T041215Z-207bf286
```

It records two prompts, two warmups, six measured generations, three real
Provider roles, eight generated tokens per result, complete decoded answers,
one durable request/response event per generation, and no per-token wire
requests. The v7 deadlock and v8/v9 validator failures remain retained as
diagnostic evidence; neither is promoted as a pass.

The old diagnostic token loop remains available only through
`--diagnostic-token-loop`. It is useful for transport/failure diagnostics but
must not be used to claim the `GenerationRequest` application contract.
For source compatibility, direct unit fixtures that construct the legacy
helper's minimal argument object without this flag still exercise that
diagnostic loop; the real CLI parser always supplies the flag and therefore
defaults to FULL.

TigerCluster requalification is a separate external-validity check. It may
reuse the qualified SIF and stage artifacts only after this local gate and must
apply the same single-invocation lineage checks; it does not alter the local
correctness claim.

## TigerCluster external-validity result

The authorized 0.6B requalification reused the existing SIF and
content-addressed stage artifacts on three distinct RTX 5000 nodes. No
foundation image or model preparation was repeated. Jobs 181946 and 181947
reached ACK closure, deferred Repo publication, Selection, and all three CUDA
stages, but exposed a client boundary error: the automatic path called
`AutomaticInferenceHandle.result()` and then read `.payload` from its decoded
`bytes` result. The retained raw row is explicit:

```text
error="'bytes' object has no attribute 'payload'"
```

The one-line fix calls `handle.response()` when the transport layer needs the
raw `ServiceResponse`. Job 181948 then produced the complete expected answer:

```text
job=181948
sourceSha256=3cee5d3d87cc9e0c48a322c4f0bf9b6f6f3bf9dc9d1450a6f5e89fe02b858194
status=OK exactReferenceMatch=true generatedTokenCount=47 stopReason=EOS
wireRequestCount=1 tokenRequestCount=0 cpuFallback=false
nodes=itiger09,itiger10,itiger11
```

The launcher retained `state=FAIL` only because its old analyzer required an
artifact marker not emitted by the sealed runtime and counted the stage-0
terminal marker once per token. The corrected analyzer was run independently
against the immutable 181948 partial evidence and returned `status=PASS`, with
47 full-progress events for every role and Repo fetch completion for all
three stages. The launcher result is not rewritten; the evidence distinguishes
runtime success from analyzer-tool failure.

This external result supports the API decision: a complete token sequence is
one durable NDNSF-DI invocation. Internal hidden-state/KV/token steps remain
data-plane work under that invocation; they do not create per-token Requests,
ACK windows, planning calls, or Selections. The normative wire result is one
complete Response; any presentation-only rendering happens after that result
and is outside NDNSF-DI collaboration semantics.
