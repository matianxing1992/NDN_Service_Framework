# Spec187 Design-Code Convergence — 2026-09-15 r1

**Task**: T006 Design-code convergence and final evidence
**Status**: PASS (static convergence only)
**Scope**: current handoff/build/materialize/run chain and the T002 native
MiniNDN caller wiring; no SIF or Tiger operation was started.

## Requirement-to-code map

| Requirement lane | Current source and evidence | Static result |
| --- | --- | --- |
| Candidate closure and mutation | `prepare-development-handoff.py`, `build-local-sif.sh`, `build-sif-app.py`, `validate-sif-app.py`, `run-sif-app.sh`; [b187-local-closure.md](b187-local-closure.md) | PASS; existing entrypoints remain authoritative and changed base is rejected before publication side effects |
| Native production caller | `CaseRuntimeBinding::process_specs()` in `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` starts `spec187-yolo-minindn` as the MiniNDN User only when `SPEC187_NATIVE_MODE=1`; [b187-local-yolo.md](b187-local-yolo.md) | PASS; no post-run or independently addressed DummyClientFace selector remains in the runner |
| MiniNDN transport and lifecycle | `start_processes()` assigns each child `NDN_CLIENT_TRANSPORT`; C++ `NativeRequesterThroughMiniNdn` calls `Runtime::open`, `user().prepare`, `PreparedModel::request`, writes the result and drains Runtime | PASS; selector/config/input/output and output collision fail before `start_network()` |
| Build/source registration | `tests/wscript` target `spec187-yolo-minindn` uses `di_integration_sources` plus the maintained integration fixture and `SPEC187_YOLO_SELECTOR` | PASS; normal target linked in `build-spec185-b0c-normal` |
| Evidence and status boundaries | `tasks.md`, [b187-local-yolo.md](b187-local-yolo.md), [qwen-deferred.md](qwen-deferred.md) | PASS; correlated C++ stage evidence, compile, independent fixture, missing-input failure, unobserved SIF/MiniNDN/Tiger and QWEN TODO are separated |

## Review and checks

- Frozen snapshot `.codex-tmp/spec187-stage-evidence-review-20260915-r7/`, diff SHA256
  `49625e235a1b2a5cd19b9f188e353ac27812c4dd002448cee899b6f18a3ce996`.
- Official read-only `review-agent`: **STATIC_PASS** after correlated marker,
  attempt/plan matching and epoch-order repairs; no P0–P2 finding remained.
- `verify-spec-kit-sync.py --require-entrypoints`: **PASS 11/11**.
- Focused Python checks: **2 passed**.
- `spec187-yolo-minindn` compile/link: **rc=0**, `-j4`, 1m6.118s; the first
  attempt's `CollaborationPlan::planDigest` compile miss is retained in
  `.codex-tmp/spec187-t002-build-r2.log` and was fixed before this result.
- Independent served-provider C++ selector: **rc=0**, `*** No errors detected`,
  `.codex-tmp/spec187-t002-selector-r3.log`.
- Native through-MiniNDN selector without required config: expected fail-closed
  `SPEC187_NATIVE_REQUEST_CONFIG` assertion, observed rc `201` in
  `.codex-tmp/spec187-t002-native-selector-missing-input-r2.log`; no network
  side effect was attempted.

## Closure decision

The design and source wiring are converged for the implemented T002 slice, so
T006 may close as a static audit. This does not promote T003 or T004: a regular
base SIF, host-gate manifest and candidate-bound native requester config/input
are still absent. Formal local `LOCAL_PASS` requires two real MiniNDN runs and
the same pair identity; TigerCluster remains downstream of that result.
