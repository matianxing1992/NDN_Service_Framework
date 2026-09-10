# Spec182 R11-B1 Independent Artifact Authority

## Result

**Batch**: R11-B1 Independent Authority

**Date**: 2026-09-10

**Baseline**: `ffc05923` (`spec182: prioritize independent authority and native process validation`)

**Status**: `PARTIAL`
**Closure decision**: `OPEN_FOR_NEXT_BATCH`; R11-B2 remains gated until the missing process case is recorded.

本批把 artifact policy/crypto 从 standalone requester 的进程内组合移到独立 C++ authority
入口。requester 只保留自身签名私钥、authority 公钥和 authority service address；authority
持有签发私钥、model content key、Provider recipient registry 与 immutable publication
policy。Python 未参与本批 native 行为证明。

## Five-Lane Coverage

| Lane | Covered source and proof | Result / gap |
| --- | --- | --- |
| production entry/callers | `examples/DI_NativeRequester.cpp`; `NativeAuthenticatedGrantClient::acquire`; `issueThroughCore`; `examples/DI_NativeArtifactAuthority.cpp` TargetedOnly handler | requester local issuer/private-content-key path removed; independent authority entry is built; real process request still open |
| implementation/wire | `NativeGrantAuthorityRequest`; `nativeGrantAuthorityRequestJson/fromJson`; `nativeKeyGrantJson/fromJson`; Core `RequestServiceTargeted` adapter; authority identity/requester/expiry/manifest checks | canonical request/response envelopes and fail-closed handler implemented; Core network interaction not yet observed |
| test/harness/oracle | C++ `Spec182NativeAuthority/*`; existing issuer and placement suites; full `Spec182*` unit selector; 9-case native integration selector | C++ primary tests pass; no independent PID/authority positive-negative process oracle yet |
| build/source closure | Waf targets `DI_NativeArtifactAuthority`, `DI_NativeRequester`, `unit-tests`, `integration-tests`; new wire translation unit is linked; `nm`/`readelf` inspect authority symbols and RUNPATH | same-source build passes; host-bound RUNPATH is retained and is not deployment qualification |
| migration/evidence | requester rejects `authority_private_key_file`, `content_key_file`, `content_key_id`; requester/authority configuration contracts; task/plan/audit links | configuration boundary documented; R11-B2 and T016 process evidence remain open |

## Static Review and Corrections

Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `ffc05923`.
The bounded review traced requester → Core authority service → issuer → requester verification →
Core publication and checked ownership, timeout/cancellation, response parsing, service permission
bootstrap, Waf registration, and test selectors.

Two defects were caught during the batch and repaired:

1. The requester CLI used `epoch` after removing the old local issuer block. The target build caught
   the undeclared symbol before any runtime claim.
2. The compatibility local-issuer constructor called the issuer with wall-clock `nowMs()` instead
   of the injected test/production `Clock`. The complete C++ selector caught
   `DI_PROTECTED_GRANT_REJECTED: issuer answer authentication failed`; the issue callback now uses
   the same clock as post-issue verification.

`git diff --check` passed. Cppcheck 1.90 was attempted on the changed C++ paths but stopped at its
vendor nlohmann/json/Boost preprocessor boundary (including unsupported `--enable=error` on this
version); that diagnostic is preserved in `.codex-tmp/spec182-r11-b1-authority-cppcheck.log` and
is not counted as a static pass. Compiler diagnostics, source tracing, targeted searches and C++
tests are the replacement checks for this batch.

## Verification

All commands used the system-first path `/usr/bin:/bin:/usr/sbin:/sbin` and `-j2` after the
host's recorded swap pressure.

```text
./waf -o .codex-tmp/spec182-r11-b1-authority-build build \
  --targets=DI_NativeArtifactAuthority,DI_NativeRequester,unit-tests,integration-tests -j2
-> PASS; final build 33.688s

After the authority secret-buffer cleansing adjustment, the same four targets were rechecked
incrementally from the current source and returned PASS in 2.233s.

.codex-tmp/spec182-r4-b2/build/unit-tests --run_test='Spec182NativeAuthority/*'
-> PASS; 3 test cases

.codex-tmp/spec182-r4-b2/build/unit-tests --run_test='Spec182*'
-> PASS; 256 test cases, 31.436132s

.codex-tmp/spec182-r4-b2/build/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182*'
-> PASS; 9 test cases, 50.026938s

DI_NativeArtifactAuthority --help
DI_NativeRequester --help
-> PASS; both native CLI usage paths returned 0
```

The complete C++ selectors are regression evidence for the changed native client and wire, not
proof of independent authority transport. No Python test, `py_compile`, fake ACK, or in-process
authority test is used to advance this native status.

## Remaining Exit

R11-B1 requires one fresh C++ process run with distinct requester/authority PIDs and key directories:

- positive request: authority receives the authenticated envelope and returns a grant that the
  requester verifies and publishes;
- negative requests: wrong recipient/signature/epoch, authority rejection or timeout fail closed;
- isolation: requester cannot read or inherit authority private/content key material.

Until those observations are recorded, R11-B1 is `PARTIAL`, T005 remains `PARTIAL`, and the next
implementation work is the process driver/fixture needed to close N1 before R11-B2.

## Batch Retrospective

| Miss class | Observation | Action |
| --- | --- | --- |
| static review miss | injected clock mismatch was semantic and escaped source review | keep injected clock and run the full C++ selector before closing a grant batch |
| compile miss | removed local issuer code left an undeclared `epoch` | build all changed native entry points in the batch |
| runtime/process miss | no real authority PID/transport was available in this batch | make the independent process driver the next bounded exit, not another broad audit |
| batching miss | authority wire, requester seam and service entry share one stable contract | retain this grouping; do not absorb Provider unary/stateful behavior into R11-B1 |
