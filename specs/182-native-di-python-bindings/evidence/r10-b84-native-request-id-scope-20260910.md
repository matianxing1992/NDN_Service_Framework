# R10-B84 Native Request Identity Scope

## Batch result

`CLOSED_FOR_VALIDATION` applies only to the native request-identity composition
boundary. The native C++ requester now adds a process-local owner scope to
production Core request names. The private C++ test port keeps deterministic
counter-only names so existing state-machine assertions remain stable. Python
is secondary in this batch: no Python implementation or Python test was used
to establish native request behavior.

## Scope and source baseline

- **Batch**: `R10-B84`
- **Baseline**: `4762fe0f2e2348aeb5f86d894e8bb0ac92055fcf`
- **Changed source**:
  - `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp`
  - `NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp`
  - `tests/unit-tests/di-native-client.t.cpp`
- **Open parent gates**: `T010`, `T011`, `T013`, `T014`, `T015`, `T016`, `T017`.

The former production shape `/NDNSF/DI/REQUEST/<counter>` used one process-wide
counter and could collide when two requester processes shared an NDN identity.
`NativeInferenceClient` now creates one 128-bit hexadecimal owner scope per
process using `RAND_bytes`; a monotonic-clock/PID fallback is retained only for
uniqueness if OpenSSL random initialization is unavailable. Production names
are `/NDNSF/DI/REQUEST/<32-hex-owner-scope>/<counter>`, and recovery IDs inherit
that base operation identity.

## Five-lane static review

The official review protocol was loaded from
`/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`). The
project review references were also checked (`review-agent.md` SHA-256
`1974ac9454ca727b264e22c391d5c98e1663622e31ca01825b12a67ad34cc16b` and
`pre-test-static-review.md` SHA-256
`f59753415869168ee469239bcb12b546101375d976bac7061e834062e8556c6d`).

| Lane | C++ evidence | Result |
| --- | --- | --- |
| Production entry/callers | `NativeInferenceClient` production constructors and `request()`; standalone requester and binding remain callers of this native owner | The owner scope is allocated in the native client, before Core/ACK/attempt identity is published |
| Implementation/wire | `processRequestOwnerScope()`, `m_requestOwnerScope`, and request-id construction in `NativeInferenceClient.cpp` | Static review found no introduced control defect; test-port compatibility is explicit and bounded to the private port |
| Test/harness/oracle | `Spec182NativeRequestIdentity/ProductionRequestIdsCarryProcessOwnerScope`; existing `Spec182ClientState/*`; full `Spec182*` C++ selector | The new C++ identity case and existing native state cases pass; Python is not the behavior authority |
| Build/source closure | Waf `unit-tests` target with system-first PATH and `-j2`; changed translation unit registered in the target | The unit-test target built successfully from the current native source tree; no unregistered source was found |
| Migration/evidence | request identity is only native-owned; SA-02/SA-03/SA-04/SA-05 findings remain linked to the next production-chain batches | This batch closes only identity scope; no caller retirement, worker/process transport, or deployment qualification is claimed |

The changed translation units were checked with Cppcheck (`warning,style,
performance,portability,inconclusive`, C++17, system include suppression). It
returned zero; the only diagnostics were three pre-existing STL-style notes in
unchanged `NativeInferenceClient.cpp` lines. The broader maintained C++ scan
also returned zero after excluding the known third-party header syntax noise.
`git diff --check` passed.

## Native C++ validation

Commands and observed results:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  ./waf -o .codex-tmp/spec182-r10-b84-request-id-build build \
  --targets=unit-tests -j2
-> PASS; unit-tests target completed (Waf reused the existing
   .codex-tmp/spec182-r4-b2/build output and rebuilt the changed source)

.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182NativeRequestIdentity/*' --log_level=test_suite
-> PASS; 1 C++ case, no errors

.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182ClientState/*' --log_level=test_suite
-> PASS; 18 C++ cases, no errors

.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182*' --log_level=test_suite
-> PASS; complete Spec182 C++ selector, Boost log ends with
   `*** No errors detected` (testing time about 128.7 s)
```

The test list contains the new `Spec182NativeRequestIdentity` suite. The
production constructor path was exercised by constructing two native clients
and checking the URI prefix, exact 32-character lowercase hexadecimal owner
scope, and distinct request names. A separate two-process executable run was
not performed in this batch; the cross-process transport gate remains open.

## Retained findings and next exit

The request identity fix does not resolve the whole-chain findings from
R10-B82/R10-B83:

1. `DI_NativeRequester` still composes an artifact-authority private key and
   local issuer; the authority boundary must be made explicit and independent.
2. Sixteen maintained Python inference call sites still use compatibility or
   automatic-planner routes. Their C++ replacement remains a production
   migration task; Python wrapper checks cannot close it.
3. No independent `DI_NativeRequester` ↔ `di-native-provider` worker-process
   run has observed unary, stream, continuation/recovery, cleanup, and result
   behavior.
4. The native ELF dependency closure is still host-bound and has no container
   qualification evidence.

The next stable exit is therefore a fresh independently launched native
requester/Provider process case with artifact identity and dependency closure
recorded. No parent task is advanced by R10-B84.
