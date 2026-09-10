# Spec182 R11-B1 Independent Artifact Authority

## Result

**Batch**: R11-B1 Independent Authority

**Date**: 2026-09-10

**Baseline**: `0ec94c72` (`spec182: isolate native artifact authority`), followed by the
R11-B1 process-driver unit recorded below and included in this local checkpoint.

**Status**: `CLOSED_FOR_VALIDATION` (R11-B1 process exit only)
**Closure decision**: `CLOSED_FOR_VALIDATION`; R11-B2 remains gated until its independent
requester → Core → Provider unary exit is recorded.

本批把 artifact policy/crypto 从 standalone requester 的进程内组合移到独立 C++ authority
入口。requester 只保留自身签名私钥、authority 公钥和 authority service address；authority
持有签发私钥、model content key、Provider recipient registry 与 immutable publication
policy。Python 未参与本批 native 行为证明。

## Independent Process Results

The final fixture pre-created NDN identities once, then gave Controller, Authority and requester
separate PIB/TPM copies with role-specific private NDN keys. Each copied PIB's TPM locator was
rewritten to its local copy. The requester bwrap namespace mounted only its read-only identity
snapshot, requester configuration, system/dependency libraries and the NFD socket; Authority grant
private material and the content key were outside the namespace.

| Case | Result | First rejection boundary | Raw run directory |
| --- | --- | --- | --- |
| `positive` | `NATIVE_GRANT_PROCESS_POSITIVE`; C++ verified issued grant | Authority handler returned grant; requester verified authority signature, recipient, request binding, digest, epoch and expiry | `/tmp/spec182-r11-b1-process-1l4qdbcw` |
| `bad-signature` | `NATIVE_GRANT_PROCESS_REJECTED` | Authority handler: request signature or operator policy rejected | `/tmp/spec182-r11-b1-process-gzsnlr8i` |
| `wrong-epoch` | `NATIVE_GRANT_PROCESS_REJECTED` | Authority handler: request signature or operator policy rejected | `/tmp/spec182-r11-b1-process-9vwdzitn` |
| `unknown-recipient` | `NATIVE_GRANT_PROCESS_REJECTED` | Authority handler: recipient key is not configured | `/tmp/spec182-r11-b1-process-gkx383jc` |
| `malformed` | `NATIVE_GRANT_PROCESS_REJECTED` | Authority handler: grant authority request is not canonical | `/tmp/spec182-r11-b1-process-xt12stdx` |
| `expired` | `NATIVE_GRANT_PROCESS_REJECTED` | Authority handler: grant expiry exceeds authority policy | `/tmp/spec182-r11-b1-process-d9uhak2g` |
| `unreachable` | `NATIVE_GRANT_PROCESS_REJECTED` | C++ Core targeted transport timeout after Authority termination | `/tmp/spec182-r11-b1-process-cym_rrn6` |

Every final run emitted `NATIVE_GRANT_PROCESS_ISOLATION=PASS`. The requester store contained only
the requester private NDN key; the external grant requester private key and Authority public key were
the only grant files mounted into `/run/requester`, while Authority's grant private key and content
key stayed in the unmounted Authority directory. This is local process evidence, not N2 unary,
stream/continuation/recovery/replacement/cleanup, maintained-caller, no-Python or T016 qualification.

## Five-Lane Coverage

| Lane | Covered source and proof | Result / gap |
| --- | --- | --- |
| production entry/callers | `NativeAuthenticatedGrantClient` → `ServiceUser::RequestServiceTargeted` → independent `DI_NativeArtifactAuthority` TargetedOnly `/HELLO` handler | real Controller, Authority and C++ requester PIDs exchanged one grant over a private NFD |
| implementation/wire | `NativeGrantAuthorityRequest` canonical JSON, requester Ed25519 signature, authority policy/recipient/epoch checks, issued grant verification | positive C++ probe verifies authority signature, recipient, request binding, digest, epoch and expiry |
| test/harness/oracle | C++ `tests/standalone/spec182-native-grant-requester-process.cpp`; Python launcher only creates processes, NFD, role stores and collects markers | one positive plus six C++-decided rejection cases pass; Python does not compute protocol verdict |
| build/source closure | Waf `spec182-native-grant-requester-process` target with explicit native DI/framework/NAC-ABE/SVS/ONNX closure; separate role PIB/TPM snapshots | target build and dynamic dependency inspection pass; host RUNPATH remains development evidence only |
| migration/evidence | requester has only its grant signing key and Authority public key; Authority grant private/content keys remain outside requester mounts; role NDN private keys are reduced per snapshot | bwrap isolation marker passes for every case; R11-B2 and T016 remain open |

## Static Review and Corrections

Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, source baseline `0ec94c72`.
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

The subsequent complete three-file diff (`tests/wscript`, the C++ process probe and its external
launcher) was reviewed read-only against `AGENTS.md`, the Native-First contract, direct call sites
and Waf registration. No actionable P1/P2/P3 finding remains. The review covered request ownership
and signing, canonical wire parsing, timeout/cancel paths, role-specific PIB/TPM ownership, bwrap
mount exclusion, process cleanup, target/source registration, and the fact that Python does not
compute the grant or protocol verdict. Three earlier fixture failures were retained in the failure
log and fixed before counting final process results: shared writable PIB certificate races,
read-only requester HOME, and a stale copied TPM locator.

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

env PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf -o .codex-tmp/spec182-r11-b1-process-build build \
  --targets=spec182-native-grant-requester-process -j2
-> PASS; final target build 12.577s

python3 -m py_compile tests/standalone/run-spec182-native-grant-process.py
-> PASS

python3 tests/standalone/run-spec182-native-grant-process.py --case positive --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case bad-signature --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case wrong-epoch --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case unknown-recipient --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case malformed --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case expired --keep
python3 tests/standalone/run-spec182-native-grant-process.py --case unreachable --keep
-> PASS; one positive, five Authority-handler rejections and one Authority-unreachable transport timeout

readelf -d .codex-tmp/spec182-r4-b2/build/spec182-native-grant-requester-process
-> PASS; native DI/framework/ndn-cxx/OpenSSL dependencies and candidate RUNPATH are present
```

The first concurrent full `Spec182*` batch-end run returned rc=201 at the pre-existing V3
placement fixture; its isolated selector passed ten times and the fresh full rerun passed
256/256 cases and 7077/7077 assertions. The boundary and raw output are recorded in
`docs/failure-log.md` and `.codex-tmp/spec182-r11-b1-spec182-unit-rerun.log`; no R11-B1 production
source was changed for that fixture observation.

The complete C++ selectors are regression evidence for the changed native client and wire. The
process probe is the C++ primary behavior oracle; Python only owns process lifecycle, private NFD,
role stores and marker collection. No Python test, fake ACK or in-process authority test is used to
advance this native status.

## Remaining Exit

R11-B1's independent process exit is now recorded with distinct requester/authority PIDs and role
key directories:

- positive request: authority receives the authenticated envelope and returns a grant that the
  requester verifies and publishes;
- negative requests: wrong recipient/signature/epoch, authority rejection or timeout fail closed;
- isolation: requester cannot read or inherit authority private/content key material.

R11-B1 is closed only for this local process validation. T005 and the Spec182 parent tasks remain
`PARTIAL`; the next implementation work is the independent C++ requester → Core → Provider unary
driver required to close N2 before R11-B3.

## Batch Retrospective

| Miss class | Observation | Action |
| --- | --- | --- |
| static review miss | injected clock mismatch was semantic and escaped source review | keep injected clock and run the full C++ selector before closing a grant batch |
| compile miss | removed local issuer code left an undeclared `epoch` | build all changed native entry points in the batch |
| runtime/process miss | no real authority PID/transport was available in this batch | make the independent process driver the next bounded exit, not another broad audit |
| batching miss | authority wire, requester seam and service entry share one stable contract | retain this grouping; do not absorb Provider unary/stateful behavior into R11-B1 |
