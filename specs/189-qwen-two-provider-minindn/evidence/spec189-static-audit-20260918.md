# Spec189 Static Audit and Progress Reclassification

**Updated**: 2026-09-18 02:22 -0500
**Mode**: full / post-test-adversarial
**Verdict**: `BLOCKED_FOR_NATIVE_EXECUTION`
**Scope**: Spec189 documents, maintained MiniNDN runner, the current C++ provider/coordinator path, and attempts r01-r21.

**Review trace**: official read-only `/root/spec185_review` review returned
`STATIC_FAIL` with no P0, three P1 and two P2 findings. It read the complete
Spec189 artifacts, runner and relevant C++ source without editing, building or
spawning another reviewer. The primary audit below incorporates that review;
the affected evidence and task statuses were corrected in this checkpoint.

This audit also installs the reusable
[experiment static re-review loop](../../../skills/speckit-code-design/references/experiment-static-review-loop.md)
as the retry rule for Spec189. A future run must preserve the prior attempt,
classify its first missing production boundary, record a real `Changed gate`,
freeze the changed source/configuration/oracle/build snapshot, and pass a new
read-only review before any rebuild or MiniNDN retry.

### Loop adoption review trace

| Field | Value |
| --- | --- |
| `Skill` | `skills/speckit-code-design/references/experiment-static-review-loop.md` |
| `Skill SHA-256` | `e587e06af27ec0b80f011ec574dc5e7c9d1538fd6b527ea62f5a2c5ba53d21d2`; personal installed copy matches via `verify-spec-kit-sync.py --require-personal` |
| `Immutable base` | repository `HEAD` `6a1aaf507fa1129d2453e1b727628c22db9c17f6`; frozen 21-file changed-scope manifest is [the snapshot manifest](spec189-static-review-snapshot-20260918.manifest), with canonical-entry SHA-256 `28dadc53d2b2a44cf77ce8c44e36627b8e41644c5ae88dd5b4d41685ae2ffb54`; no unrelated worktree file was included |
| `Diff scope` | exact relative paths and per-file hashes are in `spec189-static-review-snapshot-20260918.manifest`; the manifest excludes this audit record to avoid self-reference |
| `Queries/checks` | `rg` link/path check: exit 0, `missing links=0`; `git diff --check`: exit 0; `verify-spec-kit-sync.py --require-entrypoints --require-personal`: exit 0, `PASS: 11/11 local entrypoints`; `quick_validate.py`: exit 0, `Skill is valid!`; `audit_speckit_structure.py --strict`: exit 0, `Structural verdict: PASS`, 24 FR/6 SC/10 tasks |
| `Initial review` | `/root/spec185_review` returned `STATIC_FAIL` with P1 copied-digest exception and P2 incomplete trace; both are repaired and require re-review |
| `Re-review` | `/root/spec185_review` final read-only review returned `STATIC_PASS`; it verified the copied-digest rule, manifest path and 21/21 hashes, HEAD/skill identity, command results and AGENTS consistency. No build or experiment was run. |

## Current progress

The candidate and the real topology are substantially prepared, but Spec189 has
not reached native execution. The strongest observed sequence is:

```text
candidate fence → Controller/Authority/Provider ready
→ signed ACK offers → ACK closed → two-provider Selection committed
→ protected grant verified on both Providers → no provider execution marker
→ requester stream gap
```

The r21 requester and provider log hashes are:

| Log | SHA-256 |
| --- | --- |
| `requester-0.log` | `42c94ea5f83bf28a325382c762652407b7fd01d55e9cca6ea17e8f0ce1f4ecea` |
| `provider-0.log` | `761d393b7de74cece91a145509d9faf9cf2680b6a7e4bd3bc8630e363ec9bd2c` |
| `provider-1.log` | `cff5f5af426ef0db28c22f83e0d52744d950b072bb277c4649fa097d50f1a0d6` |

This proves ACK/Selection and grant verification only. It does not prove Repo
commit, placement-bound layer fetch, assembly, ONNX execution, hidden-state
handoff, terminal response, or cleanup.

## Findings and whether static review could have helped

| ID | Severity | Finding | Static/preflight value | Current state |
| --- | --- | --- | --- | --- |
| SA-01 | HIGH | `NativeEpochCoordinator` rebuilt role edges from the legacy plan and lost the authenticated V3 endpoint digest. | A source review comparing `roleSpecFor` with `roleSpecFromSelectionProjectionV3`, plus a C++ endpoint-preservation assertion, would have found this before r18. | Fixed by the coordinator role factory; a focused regression selector is still required. |
| SA-02 | HIGH | The batch had no stable observable exit between grant verification and first provider execution. r19/r21 therefore ended as a generic requester stream gap while provider logs contained no next-stage marker. | Static review of the five-lane gate and event contract should have required `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`, `ASSEMBLY_STARTED`, `RUNNER_READY`, `EXECUTION_COMPLETED` and `TERMINAL` markers before a MiniNDN retry. | Unfixed observability/diagnostic gap; exact runtime cause remains unobserved. |
| SA-03 | HIGH | The experiment policy originally used the application-root role prefix instead of the service-scoped role prefix. | Comparing generated policy text with the controller/provider permission contract would have caught r12 without a full run. | Fixed in the runner; add a policy-scope preflight and C++ negative. |
| SA-04 | HIGH | The operator credential required `trust-root-registry-v1.json` and its key closure, while the runner initially supplied only `authority-public.pem`. | Static inspection of `NativeProtectedGrantCredentials` and runner materialization could have caught r13 before MiniNDN. | Fixed in the runner; add a candidate-closure checker. |
| SA-05 | MEDIUM | Manually copied digests caused r01, r03, r11, r14, r16 and r20 preflight failures. | A dispatch/static gate should derive every expected digest from the frozen candidate manifest/build receipt instead of accepting repeated hand-copied values. | Still a process hazard; command path should be simplified. |
| SA-06 | MEDIUM | Dynamic KV graph shape and the fixed 5-second planning budget were discovered by runtime retries (r04/r05). | Candidate preflight can inspect ONNX state names/count and compute a graph-size budget before starting MiniNDN. | Graph and budget were corrected; checks are not yet mandatory gates. |
| SA-07 | MEDIUM | Stale 1.5-GB publication files caused r08 and reduced the 12-GB host margin. | A run-scoped publication directory, free-space reservation and pre-run residue check belong in the static/resource gate. | Cleanup was performed after the failed run; durable guard is still missing. |
| SA-08 | MEDIUM | Root Python lacked `onnx`/`numpy` in r17. | The candidate closure should verify the exact interpreter/module path before privileged launch. | Worked around with `PYTHONPATH`; not yet encoded as a hard preflight. |
| SA-09 | HIGH | The named Spec189 C++ full-path oracle is listed in the plan but no source/target is registered in `tests/wscript` or `examples/wscript`. | Static source/build review would prevent production binaries from being mistaken for an independent oracle. | B189-3 remains blocked until the oracle and link closure exist. |
| SA-10 | HIGH | T008's runner has no RSS/MemAvailable/swap/Repo/materialization sampler or resource guard; termination alone is not drain evidence. | Static runner review would catch the missing resource lane before r01-r21. | T008 remains `NOT_STARTED`; historical resource samples are not reused. |

The exact failure in r21 is **not** statically established. Static review can
prove that the required post-grant markers and failure correlation are absent;
only a rerun with those markers (or a direct C++ selector) can distinguish a
coordinator entry failure, dependency fetch failure, provider callback loss,
or requester stream transport timeout.

## Five-lane coverage at this checkpoint

| Lane | State | Evidence |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester/provider reached `Runtime.open → User.prepare → PreparedModel.request`; full post-selection caller closure is not observed |
| implementation/wire | `covered-partial` | V3 projection fix and policy/credential fixes are in source; post-grant execution wire remains unobserved |
| test/harness/oracle | `gap` | no C++ full-path oracle, fetch/assembly/terminal assertion, or endpoint regression selector has run |
| build/source closure | `covered-partial` | affected DI targets built with the recorded receipt; Spec189 symbol/`nm` map and selector evidence are incomplete |
| migration/evidence | `covered-partial` | immutable candidate and r01-r21 raw logs are retained; repeat convergence and cleanup evidence are absent |

The four miss classes are:

- `static`: SA-02, SA-05, SA-06, SA-07 and SA-08 gates were not present before the runs;
- `compile/link`: the affected DI build passed, but no Spec189 C++ oracle target has run;
- `runtime/test`: ACK/Selection and grant verification are observed; execution and terminal lanes are not;
- `unobserved`: the first boundary after grant verification, Repo commit ownership, layer fetch, assembly, hidden-state handoff and drain.

## Required Spec189 corrections

1. B189-0 must freeze a candidate preflight that checks policy scope, operator
   registry closure, dynamic KV shape, planning budget, Python module closure,
   run-scoped disk reservation and all derived digests.
2. B189-3 must stop at named exits, in order:
   `GRANT_VERIFIED → EXECUTION_ENTERED → DEPENDENCY_FETCH → ASSEMBLY_STARTED
   → RUNNER_READY → EXECUTION_COMPLETED → TERMINAL`. A requester stream gap is
   only a transport symptom until the provider-side first missing marker is
   classified.
3. T006/T007 must include a C++ endpoint-preservation regression and provider
   execution-entry/failure oracle. A static review cannot substitute for the
   runtime handoff assertion.
4. T009 must use a candidate-derived digest lock and fail before MiniNDN when
   the policy, credential closure, interpreter or residue check is wrong.
5. T010 remains `NOT_STARTED` until a second run uses the same immutable tuple
   and reaches the same complete terminal/cleanup classification.
6. Register the C++ full-path oracle and its Waf source/link closure before
   calling B189-3 runtime evidence complete; production `DI_NativeRequester`
   and `di-native-provider` binaries are not independent oracle targets.
7. Implement T008's C++ counters and maintained sampler/guard before treating
   child termination or a `finally` cleanup path as resource evidence.

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: the post-grant execution markers and C++
endpoint regression selector are implemented and reviewed, then a fresh run
observes either the first provider-side failure boundary or the complete
fetch/assembly/execute/terminal sequence. No task may be marked `[x]` from the
current r21 evidence.
