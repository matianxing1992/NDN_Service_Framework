# Spec182 R10-B82 Whole-Chain Static Audit

Date: 2026-09-10
Branch: Experimental
Source baseline: `e596b645`
Decision: OPEN_FOR_NEXT_BATCH
Boundary: large read-only implementation-distance review; no parent task advanced

## Scope and coverage

| Lane | Covered boundary |
| --- | --- |
| Production entry/callers | CodeGraph trace from `NativeInferenceClient::request` through `dispatchOperation`/`beginCoreRequest` to Core `BeginCollaboration`; standalone `DI_NativeRequester`; `di-native-provider`; maintained Python caller inventory. |
| Implementation/wire | Request identity allocation, native requester configuration, grant acquisition/publication, conversation ownership, Provider worker/serve lifetime, stream and cleanup paths, and Waf source/target registration. |
| Test/harness/oracle | C++ unit/integration selectors, Python AST inventory, Cppcheck 1.90, the prior changed-TU Clang analyzer pass, `nm`/`readelf`/`ldd`, and existing requester/Provider probes. |
| Build/source closure | Current requester and Provider artifacts, target source lists, RUNPATH and resolved dependency roots. |
| Migration/evidence | Compatibility manifest inventory and the R10-B81 evidence record; no source or runtime qualification claim is inferred from card counts or local selectors. |

## Prioritized findings

- **SA-01 HIGH — OPEN: documented native conversation configuration is ignored by the standalone C++ requester.** `native-requester-configuration.md` requires an optional `conversation` object whose journal/key files are read by C++ and whose coordinator is injected into `NativeInferenceClient`. `examples/DI_NativeRequester.cpp` validates catalog/grant/request/runtime fields but never reads `config["conversation"]`; it constructs the five-argument runtime client without a `NativeConversationCoordinator`. A configured continuation therefore cannot run through this entry point, while an unconfigured continuation correctly fails closed. The Python binding has a separate coordinator loader, so this is an entry-point mismatch rather than proof that the native coordinator itself is absent.
- **SA-02 HIGH — OPEN: requester placement still contains the artifact-policy authority private key and issuer.** The standalone requester reads `authority_private_key_file`, constructs `NativeArtifactGrantIssuer`, and passes it into `NativeAuthenticatedGrantClient`; `acquire()` then calls `m_issuer->issue(...)` locally before publishing through Core. The symbol contract says `NativeArtifactPolicyAuthority` is independent of requester placement. This is an interim local composition and leaves the production authority boundary/transport unimplemented; it must be resolved or explicitly constrained before deployment qualification.
- **SA-03 HIGH — OPEN: maintained caller migration is incomplete.** The R10-B81 Python AST/manifest inventory still records 16 actual maintained public inference calls using compatibility or automatic-planner APIs. Native helper methods and local compatibility tests do not prove default-route migration or legacy zero-use. T013-B/T013 and the no-Python gate remain open.
- **SA-04 HIGH — OPEN: no independent requester → Core → Provider worker/process qualification exists.** Existing R10-B* selectors exercise in-process Core/Provider fixtures and bounded Provider startup/serve behavior. There is still no fresh independent requester and Provider process run observing unary, stream, continuation/recovery, cleanup, and terminal result together. Cross-process transport and two-round conversation recovery therefore remain unverified.
- **SA-05 HIGH — OPEN: candidate dependency closure is host-bound and inconsistent.** `readelf`/`ldd` on the current requester and Provider artifacts resolve NAC-ABE/SVS/ONNX Runtime and other NDNSF libraries from host locations. The two artifacts also resolve `libndn-cxx` from different roots. This is compatible with local development probes but does not establish relocatable/container source, ABI, RPATH, or library-hash closure required by T014/T016.
- **SA-06 MEDIUM — OPEN: native request IDs use a process-local counter.** `NativeInferenceClient` allocates `/NDNSF/DI/REQUEST/<counter>` from a process-local atomic. Core request naming later binds requester identity, but the native allocation itself is not globally unique across simultaneous requester processes using the same identity. Either the ownership/identity scope must be made explicit or a cross-process uniqueness test must be added before treating the worker boundary as closed.
- **SA-07 LOW / TOOL WARNING — REVIEWED, NOT CONFIRMED.** Cppcheck reports constructor-time execution in the final `OnnxRuntimeModelRunner` and a possible null promise in `ProviderRoleWorker::failPromise`; current ownership/call-site tracing makes both reachable product failures unconfirmed. Pass-by-value/style and valid-lambda parser diagnostics are low-value tool noise. The broad Cppcheck command exits 0 and the prior changed-TU Clang analyzer pass reports no diagnostics.

## Distance from complete implementation

The repository has substantial local implementation: native catalog/preparation, sealing,
grant, stream, conversation state and Provider registration boundaries have focused C++
tests, and bounded in-process unary/stream/Qwen/conversation selectors pass. That is not yet
the complete product behavior. The explicit fail-closed `NATIVE_REQUEST_PIPELINE_NOT_READY`
path still exists when the full runtime/preparation/encoding chain is not supplied, 16
maintained inference callers remain on old routes, the standalone C++ conversation entry is
not wired, authority placement is not separated, and independent worker/process,
cross-process recovery, no-Python and deployment-closure qualification are absent. Therefore
T010/T011/T013/T014/T015/T016/T017 remain open or partial; no defensible completion
percentage can be derived from the 17 parent cards or 40 execution cards.

## Validation and next stable exit

The static lanes completed without a new confirmed algorithmic correctness defect. Existing
R10-B81 integration/unit builds and focused selectors remain the relevant local behavior
evidence; this batch did not rebuild or run a protocol qualification campaign. The next
bounded batch should first repair or explicitly redesign the standalone conversation and
authority boundaries, then launch one independent requester/Provider process case with fresh
artifact identity and source/dependency closure. That case must record the first failure
boundary and terminal result before adding caller migrations or broader qualification cases.

`STATIC_PASS` applies to the audit boundary only. The closure decision is
`OPEN_FOR_NEXT_BATCH`; this record does not promote any parent task or T016 qualification.
