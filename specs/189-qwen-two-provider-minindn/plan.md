# Implementation Plan: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Branch**: `189-qwen-two-provider-minindn` | **Date**: 2026-09-18 | **Spec**: [spec.md](spec.md)

## Summary

Spec189 adds a real, separately auditable Qwen3-0.6B two-provider experiment. `prepare` is the durable model boundary: it validates the canonical graph and initializer, performs the layer split once, publishes immutable manifest/layer references to Repo, and returns a reusable `PreparedModel` handle. Every later `request` carries only that reference and input. Core ACK/Selection binds the request to two provider placements; only after Selection do providers fetch their assigned layer packages and assemble native runners.

The experiment is a diagnostic/product-target gate, not a replacement for Spec188's bounded core. It must use the maintained C++ authority/requester/provider targets and MiniNDN harness. Python is limited to process orchestration, host sampling and evidence collection.

## Technical Context

**Language/Version**: C++ production/runtime and selectors; Python 3 orchestration only

**Primary Dependencies**: NDNSF Core, NDNSF-DI, NDNSF-Repo, ndn-cxx/NFD, NDN-SVS, NAC-ABE, ONNX full-protobuf/Runtime CPU, MiniNDN. Host validation uses one installed global dependency closure: Boost 1.71 from the system pair and the remaining direct NDNSF libraries/archives from `/usr/local`; a checkout or `.codex-tmp` prefix is a preflight failure.

**Storage**: native Repo manifest plus immutable canonical graph, initializer and two layer packages; local filesystem staging is an implementation detail and is not a request payload

**Testing**: named C++ selectors (`DI_NativeArtifactAuthority`, `DI_NativeRequester`, `di-native-provider`, `DI_NativeOnnxAssemblyWorker`), C++ full-path oracle, MiniNDN two-provider runner, resource sampler, optional ASan/UBSan

**Target Platform**: current Linux host, 6 logical CPUs, 12 GiB RAM; root MiniNDN execution with `/usr/sbin:/sbin` in PATH

**Project Type**: native distributed-inference runtime experiment with Repo-backed model preparation

**Performance Goals**: one prepare can be reused by multiple requests; no request-time canonical publication; two providers are active for one request; resource guard prevents unsafe host exhaustion

**Constraints**: exact Qwen3-0.6B snapshot, two providers only, two-stage initial split (0–14 and 14–28), no synthetic ACK/Selection, no preloaded whole-model runner, no SIF/Tiger dispatch

**Scale/Scope**: one local input and one repeat under a fresh run id; full numerical quality campaign and cluster scale are out of scope

## Constitution Check

PASS for the planned local experiment, subject to these gates:

- Native preparation, Repo publication, wire/placement, provider execution, lifecycle and output assertions are C++ owned.
- Each logical batch has a stable observable exit, immutable review snapshot, five-lane coverage record and first-failure evidence.
- MiniNDN is downstream of code-aware convergence and affected C++ build/source closure.
- SIF/Tiger is not started by this Spec and cannot be inferred from a local run.
- A resource stop is an observed diagnostic boundary, not a PASS or a protocol failure.

## Architecture Decisions

### AD-01: `prepare` owns durable model publication

The public preparation entry resolves the pinned Qwen snapshot, creates the canonical graph/initializer identity, splits the 28 layers into two immutable packages, validates digests and writes one Repo manifest/reference. The returned handle owns only identity and lease/reference state. A second request reuses the committed manifest; it does not publish the model again.

### AD-02: `request` is reference-only

`PreparedModel::request(Input, RequestOptions)` encodes model reference, manifest digest, protection epoch, input reference and request metadata. It rejects full-model bytes and arbitrary model URLs. The request does not cause Repo ingest or canonical source publication.

### AD-03: authorization and placement precede heavy fetch

The Core ACK/Selection path binds two provider identities and exact ranges to the prepared manifest. Providers may inspect summaries before Selection, but only a valid Selection can authorize package fetch and runner creation. Cache reuse is allowed only after rechecking request identity, grant/epoch and placement.

### AD-04: each Provider owns only its selected materialization

Provider-0 owns layers 0–14 and Provider-1 owns layers 14–28 for the initial candidate. Each provider verifies package digest/range/role, assembles an ONNX runner from its package, executes, and releases the runner/lease after terminal or cancellation. No provider receives a hidden whole-model fixture.

### AD-05: hidden-state handoff is an explicit production boundary

The plan must identify the current production handoff symbol and wire fields. If the existing requester/provider path cannot carry hidden state between the two stage runners, the C++ task records `PROTOCOL_BOUNDARY` and stops; it must not replace the path with a Python or in-process shortcut.

### AD-06: evidence identity is immutable

The candidate tuple includes source commit, ABI/library hashes, build tree, model files, canonical graph/initializer/layer digests, profile, selector hashes, topology and run id. Changing any tuple member invalidates all later evidence.

### AD-07: post-grant execution has explicit observable exits

`ACK`/`Selection` and protected-grant verification are admission evidence, not
execution evidence. Each Provider must expose the ordered exits
`GRANT_VERIFIED`, `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`, `ASSEMBLY_STARTED`,
`RUNNER_READY`, `EXECUTION_COMPLETED` and `TERMINAL`. A requester-side
`stream event gap` is only a transport symptom; classification must name the
first missing Provider exit or preserve the result as `UNOBSERVED`.

## Production paths and design-to-code binding

| Binding | Production symbols / files to inspect | Required proof |
| --- | --- | --- |
| `Q189-PREP` | `Runtime::prepare`, `NativeCanonicalPreparationCatalog`, `NativeCanonicalArtifactPublisher`, `PreparedModel` | one Repo commit, reusable reference-only handle, no source retention beyond declared owner |
| `Q189-REPO` | `RepositorySourceProvider`, `RepoCore`/file backend, `RepoTypes` manifest/reference | graph/initializer/layer digest and range lookup; duplicate prepare is a hot hit |
| `Q189-WIRE` | `NativeRequestEnvelope`, `NativeRequestPreparation`, Core request/ACK/Selection path | request has reference only; ACK then signed two-placement Selection |
| `Q189-ASSEMBLY` | `NativeCanonicalOnnxAssembler`, `NativeOnnxAssemblyWorker`, `di-native-provider` | selected range only, digest-before-ORT, runner creation count and cleanup |
| `Q189-HANDOFF` | requester/provider handoff symbols discovered with CodeGraph and `nm -C` | hidden-state identity/range/attempt binding; no local shortcut |
| `Q189-ORACLE` | `DI_NativeArtifactAuthority`, `DI_NativeRequester`, `DI_NativeOnnxAssemblyWorker`, `di-native-provider`, MiniNDN wrapper | C++ event sequence and terminal output/failure |
| `Q189-EVIDENCE` | `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`, evidence checker and `.codex-tmp` run dir | resource samples, child cleanup, immutable tuple and first boundary |

`Q189-ORACLE` is currently a design binding, not an existing target: the
production binaries listed above are not an independent Spec189 C++ oracle.
Before B189-3 can close, the oracle source must be registered in
`tests/wscript` or `examples/wscript`, linked against the same DI source
closure, and run with named assertions for endpoint identity, provider stages,
terminal output and drain.

## Logical Batch Quality Plan

| Batch ID | Stable observable exit | Members | Dependencies | Shared selector/owner | Five-lane coverage scope | Risk/profile/invariant | Result record |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `B189-0` | source, model, ABI, split and handoff contract frozen | T001 | Spec189 docs | main agent + CodeGraph | production callers, contracts, selectors, Waf targets, evidence paths | medium / none / no implementation | `evidence/b189-convergence.md` |
| `B189-1` | prepare commits one reusable two-layer Repo reference | T002,T003 | B189-0 | `DI_NativeArtifactAuthority` C++ selector | `Runtime::prepare`, publisher/Repo wire, C++ fixture/oracle, affected build closure, manifest evidence | high / asan-ubsan / one commit and source release | `evidence/b189-prepare.md` |
| `B189-2` | request→ACK→Selection creates two valid placement bindings | T004,T005 | B189-1 | `DI_NativeRequester` plus Core controller; C++ wire oracle | envelope fields, ACK/Selection callers, parser/oracle, target/source map, placement evidence | high / none / no fetch before Selection | `evidence/b189-placement.md` |
| `B189-3` | `GRANT_VERIFIED → EXECUTION_ENTERED → FETCH → ASSEMBLY → RUNNER_READY → EXECUTE → TERMINAL` on two native Providers | T006,T007 | B189-2 | `di-native-provider`, `DI_NativeOnnxAssemblyWorker`, C++ end-to-end selector | provider callers, package/NDN handoff, C++ driver/oracle, link closure, execution evidence | critical / asan-ubsan where feasible / two runner and one terminal | `evidence/b189-execution.md` |
| `B189-4` | resource guard and deterministic cleanup classify success/stop | T008 | B189-3 | C++ counters + Python sampler | ownership symbols, cleanup wire, C++ lifecycle oracle, runner build, resource evidence | critical / none or asan-ubsan / no stale lease/runner | `evidence/b189-resource.md` |
| `B189-5` | immutable candidate evidence and repeat verdict are consistent | T009,T010 | B189-4 | evidence checker + same native selectors | all five lanes and hash/event checks; no cross-candidate reuse | high / none / verdict requires full event sequence | `evidence/b189-convergence.md` |

Batch growth decision: stop each batch at the stated exit. Do not add SIF, cluster, broad model quality, general Repo redesign, or unrelated Python facade work to save a build. A changed handoff, source closure, or provider protocol creates a new batch and invalidates later evidence.

### Audit correction after r01-r21

The first real runs exposed that the original B189-3 exit was too coarse. The
next B189-3 attempt must first pass candidate preflight for service-scoped role
policy, protected-grant registry/key closure, dynamic KV shape, graph-size
budget, privileged Python modules, run-scoped disk reservation and derived
digests. It must then freeze the post-grant marker sequence from AD-07. This
does not claim that the current production path emits all markers; adding and
testing the missing markers is part of T006/T007.

### Experiment retry review cadence

Every new MiniNDN or native-selector attempt uses the shared
[experiment static re-review loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md).
For the next retry, freeze r21 and the candidate tuple, classify the first
missing Provider exit, and record the Changed gate covering the C++ endpoint
regression, post-grant markers, C++ oracle registration or resource guard.
The review-agent then reads an immutable snapshot of the changed caller,
configuration/policy, fixture/oracle, build/source closure and negative path.
Only a clean re-review permits the affected target to be rebuilt and the
bounded experiment to be rerun. Retry-only changes do not advance B189-3.

## Immutable candidate and invalidation matrix

| Plane | Identity | Invalidates | Restart gate |
| --- | --- | --- | --- |
| Source | Experimental commit + changed-file digest | all native binaries/evidence | B189-0 |
| ABI | compiler/binutils, Boost 1.71, NDN-CXX/SVS/NAC-ABE/ORT SONAME and hashes | all linked targets | B189-0 |
| Model | Qwen snapshot/revision, tokenizer/config, graph/initializer/layer digests | Repo manifest, placement and runs | B189-1 |
| Split | layer count, ranges, stage schemas and tensor contracts | both provider packages and Selection | B189-1 |
| Profile | nodes, budgets, input and resource floors | MiniNDN/resource evidence | B189-2 |
| Harness | script, selector, topology and oracle | run result | B189-2 |

## Formal validation order

1. Read newest failure boundary, architecture guide and current Spec189 docs; inspect actual symbols with CodeGraph.
2. Freeze each task snapshot and run read-only review-agent before dependent implementation.
3. Run candidate preflight: derive all digests from the frozen record, validate the policy/credential/module closure, inspect ONNX state names and graph-size budget, reserve run-scoped publication space and reject residue.
4. Build only affected native targets with the matching installed global dependency closure; verify real paths and SHA-256 before compilation. Use `-j4` unless `vmstat` shows sustained swap, then use `-j2`.
5. Run the named C++ selectors and record static/build/runtime/unobserved misses separately.
6. Run MiniNDN as root with a unique run id and resource guard. The Python wrapper may stop processes, but the C++ oracle decides product behavior.
7. Repeat only with a new run id after the first result is durably recorded; do not reuse a previous PASS across an invalidated candidate.
8. Stop before SIF/Tiger. A local `QWEN_TWO_PROVIDER_PASS` is necessary evidence for a future downstream candidate, not that candidate itself.

## Closure rule

Spec189 is complete only when B189-0 through B189-5 have durable evidence, the real C++/MiniNDN path observes prepare/Repo commit, ACK, Selection, both placement-bound Provider fetch/assembly/execute paths, terminal response and cleanup, and the repeat run agrees under the same immutable candidate tuple. A resource/protocol/fixture boundary keeps the corresponding task `PARTIAL`, `BLOCKED` or `UNQUALIFIED`; it cannot be converted into PASS by a stage export or focused component selector. The current r21 state is `BLOCKED_FOR_NATIVE_EXECUTION` until the first post-grant Provider boundary is observable.
