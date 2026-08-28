# Spec 165 Post-Implementation Code-Aware Audit

## Verdict

**CURRENT STATE (2026-08-01): MININDN AND CANDIDATE CONTAINER PASS;
EXTERNAL AUTHORIZATION ELIGIBLE; NO TIGERCLUSTER DI WORKLOAD SUBMITTED.**

The strict real-model MiniNDN profile was added after the historical closure:
`--require-real-model` rejects fake runtimes and validates explicit topology,
campaign identity, all three selected roles, and per-stage ONNX readiness and
execution markers. Recheck
`results/spec165-minindn-first/20260801T204709Z-28c987c4` passed Gate B with
the reused content-addressed artifacts. Because the source revision changed,
the full Gate A-D plus candidate-container aggregate now passes at
`results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8`.
The candidate is reproducibly built by
`packaging/ndnsf-di-container/oci/Dockerfile.spec165-minindn-cpu-gate` and uses
image digest
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`,
which replaces the CUDA ONNX Runtime wheel with the CPU distribution while
keeping the same parent, source mount, model, workload, MiniNDN, and NFD.
The earlier CUDA-wheel CPU-fallback timeout and topology-path defect remain
retained as negative evidence. The canceled TigerCluster job 181811 is not a
validation result. Separate capability probe job `181812` passed on `itiger07`;
it verified Apptainer `--nv` and allocation scratch only, not Qwen or NDNSF-DI.

**HISTORICAL PASS; superseded for current-source authorization.**

The historical canonical local run is
`results/spec165-local-gates/20260731T052926Z-5debf140`: Gate A-D all pass in
one aggregate, `externalValidationAuthorized` is true, and
`tigerClusterSubmitted` is false. It is retained for regression history only
and does not authorize a separately invoked TigerCluster validation for the
changed source.

## Mandatory gate execution

| Gate | Result | Evidence |
|---|---|---|
| Context Mode | PASS | project health and active Spec 165 pointer verified; repository files remained authoritative |
| CodeGraph | PASS | current validation runner, MiniNDN harness, request-ID path, and collaboration watch inspected |
| Spec Kit local scripts | PASS | repository-local create/setup/check paths passed syntax/help and active-feature checks; strict audit passed |
| GSD | PASS | local GSD installation and health validation passed |
| ARS experiment design | PASS | material identity, experimental unit, failure accounting, and deadline design are recorded in `experiment-plan.md` |

No global `specify` executable was installed or used. Existing
`.specify/scripts`, templates, configuration, and extensions were not changed.

## Implementation and evidence

The delivered default gate is fail closed:

- Gate A validates versioned fidelity and immutable source/model/workload
  identity, and rejects missing, malformed, contradictory, stale, cross-run,
  skipped, and lower-tier substitutions.
- Gate B executes the pinned Qwen3-0.6B ONNX graph over real MiniNDN with one
  User, one Controller/security carrier, and three Provider stages.
- Gate C executes the identical workload in a bounded candidate container with
  image identity, backend, resource, exit, and OOM evidence.
- Gate D validates authenticated monotonic progress, renewable idle deadline,
  immutable hard deadline, and first-terminal-wins semantics.

The host MiniNDN and CPU candidate-container real gates each contain two
warmups and six measured invocations. Every invocation generated exactly eight
tokens and retained decoded text, TTFT, inter-token latency, total latency,
tokens/s, and token-level lineage. Gate B and Gate C each executed 64
distributed token requests. The previous CUDA-wheel candidate timeout remains
negative evidence and is not mixed into the passing aggregate.

The focused Spec 165 unit suite passed 27 tests. Task evidence is under
`specs/165-real-di-validation-gates/evidence/`.

## Request identity and progress ownership

The application creates the canonical request ID and passes it into the
application client; the client validates it, prevents active duplicates, and
does not replace it with an internal UUID. The accepted host/container
evidence has exact request-ID equality between application records and wire
transport records.

Generic progress authentication, identity binding, monotonic admission, idle
renewal, hard-cap enforcement, and terminal arbitration remain in NDNSF.
Model identity, stage phases, prompts, tokens, and generation metrics remain
in NDNSF-DI. No DistributedRepo reservation/locking semantics were introduced.

## Security and failure handling

PASS:

- evidence versions and required fields fail closed;
- stale, duplicate, reordered, forged, and wrong-binding progress cannot renew
  the idle deadline;
- the hard deadline cannot be extended by continuous progress;
- request/attempt/plan/model/provider-role mismatches are rejected;
- a passing diagnostic subset cannot authorize external validation;
- a missing image, model, backend, or real-gate record is a blocking failure;
- candidate-native packages are loaded from the candidate image, preventing a
  mounted stale host extension from masquerading as container evidence.

Negative evidence was retained. Run
`results/spec165-local-gates/20260731T052459Z-0d7ae6ec` demonstrates that an
aggregate rejects model/workload identity mismatch even when each underlying
command reports PASS.

## Reproducibility and claim boundary

The current-source closure binds source revision, candidate image ID
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`,
Qwen model digest
`sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a`,
and workload digest
`sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9`.

The six measured invocations per local environment establish functional
repeatability, not a statistically strong performance comparison. This proves
the real CPU MiniNDN path and the CPU candidate-container path; it does not
claim GPU performance, large-model performance, or TigerCluster success. The
generic GPU capability preflight is recorded in
`specs/166-spec165-itiger-validation/evidence/gpu-capability-preflight-181812.md`.
The next step is first to produce a clean current-source CUDA candidate release
and materialize its SIF; only then can a candidate-bound standalone GPU
preflight use the same immutable source/model/workload/image identity. The
local gate remains non-submitting.
