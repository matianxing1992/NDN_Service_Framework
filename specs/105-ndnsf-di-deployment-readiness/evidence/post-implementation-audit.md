# T099 Post-implementation analysis and audit

## Verdict

**BLOCK**. Spec 105 is a strong, honestly measured research prototype and a
reproducible local packaging candidate, but it is not a deployable MiniNDN
candidate. The controlling evidence is not ambiguous: fixed-load performance,
live recovery, canary/operations/soak, and historical log hygiene are BLOCK.
The code-aware audit additionally found production operator adapters and package
lifecycle behavior that are not yet executable as documented.

ARS reviewer-style decision: **major revision / do not accept deployment
claim**. The negative result and physical deferral are credible; the readiness
claim is not.

## Deterministic Spec Kit analysis

Commands:

```bash
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
codegraph sync .
codegraph explore "runtime_v1 production CLI ... packaging install rollback uninstall"
```

Structural scan: PASS; 5 user stories, 26 FRs, 11 SCs and 102 tasks; 26/26 FRs
have traceability entries (100% structural coverage); no CRITICAL constitution
conflict, duplicate ID or unresolved placeholder. `PASS|BLOCK` strings are
contract enum examples, not placeholders.

Cross-artifact analysis found two semantic consistency gaps: the operator CLI
contract is not implemented by the default adapters, and traceability still
describes SC-007 as two clean runs although T092 correctly closed as two
`NOT RUN / BLOCK` preflights after R2.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| H1 | HIGH | Code reality / task executability | `runtime_v1.py:3371-3422`; `operator-cli.md:6-20` | `provider` defaults to `--check-only`; `run` records plan/request paths but does not pass either to the harness; `bench` passes unsupported `--runner-mode` instead of the harness `--runtime`. T079 tests only dry-run command strings, so T083 was checked without executing a valid production command. | Implement typed profile-to-command adapters, execute negative/positive fixture commands, add `plan --explain`, and fail closed on unused inputs. |
| H2 | HIGH | Validation / evidence | `release-gate.json:24-55`; `telemetry-performance-check.md:44-54`; `fault-recovery.md:22-29` | SC-002/003 performance, SC-006 live recovery, SC-007/008/009 operations and soak are not met. One valid load cell completed 25/60 at 20.17x baseline p95; fault evidence has `networkInjection=false`; canary and soak are not run. | Keep candidate BLOCK. Add executable remaining tasks; never weaken or rerun the frozen T062 campaign. |
| H3 | HIGH | Migration / rollback | `install.sh:25-53`; `uninstall.sh:13-17` | Re-activating the already-current release overwrites `previous`, so idempotency can destroy a rollback point. Install does not establish dedicated accounts; uninstall leaves installed units/tmpfiles/logrotate and does not stop/disable services. | Preserve `previous` when target equals current; add account/install ownership checks and reversible unit/config removal with Repo protection tests. |
| H4 | HIGH | Security / doctor | `tools/ndnsf_runtime.py:414-490`; task T084 | Doctor checks certificate/model/trust files by existence only, does not bind certificate to identity, does not verify release/model digests or plan/evidence identity, and labels a configured age as a `linux-proc` telemetry probe without sampling telemetry. It can therefore report ready without the facts T084 claims. | Add exact certificate/identity, digest, plan/evidence and measured telemetry checks; test stale/tampered inputs. |
| H5 | HIGH | Operations | `runtime_v1.py:3439-3457`; `operator-cli.md:21-22` | `status` trusts any JSON containing `ready=true` without schema, freshness, release, plan or evidence identity. Missing metrics silently exports an empty snapshot and exits 0. This violates fail-closed unhealthy/BLOCK semantics. | Validate status schema/freshness/bindings and make absent/stale metrics nonzero unless an explicit empty diagnostic mode is requested. |
| M1 | MEDIUM | Evidence integrity | `release_gate.py:18-66`; `release-gate-input.json` | The gate normalizes caller-supplied statuses and nonempty artifact names but does not verify artifact existence/digest or derive dimension verdicts from machine-readable evidence. `sourceCommit` predates the classifier/gate commit used to generate the report. | Add evidence manifest/digest verification and record generator commit separately from candidate source commit. |
| M2 | MEDIUM | Validation | `systemd-staging.md:32-55` | Staging replaces every `ExecStart` with `/bin/true` and performs static verification because systemd 245 lacks `verify --root`. This does not validate live PID-1 supervision or real binary arguments. | Retain BLOCK and add an isolated live supervisor MiniNDN test when the earlier recovery gate permits it. |
| M3 | MEDIUM | Cross-artifact consistency | `traceability.md` SC-007/SC-009 rows; tasks T092-T093 | Traceability says two clean runs and a local drill close the criteria, while R2 records them as gated `NOT RUN / BLOCK`. | Synchronize traceability and operator docs with the final negative verdict. |
| M4 | MEDIUM | Security evidence | `security-log-audit.md` | Historical T062 INFO output contains the public expected-token oracle. Source is remediated, but the immutable run cannot prove the new format. | Keep applicationSecurity BLOCK; validate the new format only in a future independently preregistered campaign. |
| L1 | LOW | Documentation | `operator-cli.md:8`; `runtime_v1.py:3469-3477` | Contract documents `plan --explain`, but the parser has no argument or explain artifact. | Implement with H1 and synchronize help/examples. |

## Traceability gaps

| Requirement / task | Missing link | Impact |
|---|---|---|
| FR-019 / T083 | Production CLI is wired only at dry-run/string level | Operator cannot rely on documented commands. |
| FR-019 / T091 | No live service-manager execution with real binaries | Supervisor behavior remains unproved. |
| FR-021 / T088-T093 | Same-release reactivation and complete uninstall not exercised | Rollback point can be lost; uninstall is incomplete. |
| FR-022 / T084-T085 | Status/metrics do not validate freshness and identity | Stale readiness can be presented as healthy. |
| SC-006-009 | Required live/final evidence absent | Candidate must remain BLOCK. |

No unjustified new protocol or Core abstraction was found. Attempt authority,
bounded dependency waiting, typed execution evidence, telemetry separation and
physical deferral remain in the correct DI/operator ownership boundary.

## Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | YES | MiniNDN-only scope and Spec 106 physical deferral preserved. |
| Architecture and ownership | YES | No new Core wire protocol; DI owns planning/recovery. |
| Security/correctness | NO | Historical log finding and incomplete doctor/status validation. |
| Task executability | NO | Default production CLI commands are not valid adapters. |
| Validation/evidence | NO | Performance/live recovery/canary/soak BLOCK. |
| Migration/rollback | PARTIAL | Isolated N/N+1/rollback passes; idempotent previous/uninstall gaps remain. |
| Code reality | PARTIAL | Core runtime tests pass; operator claims exceed wiring. |

## Metrics and evidence limits

- Requirements: 26; structurally mapped: 26 (100%).
- Tasks at audit start: 102; checked: 98; workflow closure tasks pending: 4.
- Findings: 0 CRITICAL, 5 HIGH, 4 MEDIUM, 1 LOW.
- Full tests: C++ 242/242; Python 405 PASS with 1 skip; security and default
  MiniNDN quick suites PASS.
- Physical hosts, real GPU telemetry, real production identities and physical
  release authority were not inspected and remain Spec 106.
- No literature novelty claim was evaluated; ARS was used only to enforce
  methodology, falsification and claim/evidence boundaries.

## Remediation order

1. Repair and execute production CLI adapters (H1/L1).
2. Make doctor/status/metrics fail closed on exact measured identity (H4/H5).
3. Repair package idempotency/account/uninstall behavior (H3).
4. Make the gate verify evidence manifests and synchronize traceability (M1/M3).
5. Preserve T062 as BLOCK; run no replacement. Execute only independently
   authorized MiniNDN live-fault/operations work and keep failures (H2/M2/M4).

Post-implementation gate remains **BLOCK** until those tasks converge. This is
not a reason to move local work to Spec 106; Spec 106 remains physical-only.

## Convergence remediation status

Second pass after commit `c6aef7727ef34f16be353cd1e5aa0a143ef19f2e`:

| Finding | Status | Evidence |
|---|---|---|
| H1 / L1 production CLI and `plan --explain` | REMEDIATED | T103 executes positive/negative production adapter fixtures; every run input is a required no-shell placeholder; default bench uses valid `--runtime qwen-onnx-cpu-native`. |
| H3 package lifecycle | REMEDIATED | T105 preserves `previous` on same-release activation, records/creates dedicated accounts, removes installed supervisor assets on uninstall, and revalidates Repo preservation in isolated staging. |
| H4 doctor bindings | REMEDIATED | T104 checks certificate name/digest, trust/release/model/plan/evidence digests and identities, and fresh measured `linux-proc` telemetry; stale/tampered fixtures fail. |
| H5 status/metrics | REMEDIATED | T104 requires v1 schema, freshness and release/plan/evidence binding; missing metrics returns nonzero without output. |
| M1 gate provenance | REMEDIATED | T106 binds all 10 referenced evidence files by path/SHA-256 and records separate candidate/generator commits; missing/tampered tests pass. |
| M3 traceability | REMEDIATED | T107 records R2 canary/operations `NOT RUN / BLOCK` and links synchronized operator documentation. |
| H2 / M2 / M4 acceptance evidence | OPEN, controlling BLOCK | Performance remains failed, live network recovery/operations remain unexecuted, and immutable historical INFO evidence retains the public token oracle. No threshold or old run was altered. |

Focused convergence validation: 407 maintained Python tests PASS with one skip;
deployment/profile JSON and shell syntax PASS; evidence manifest errors `[]`;
release gate remains `minindnCandidateOverall=BLOCK` and
`physicalProductionOverall=DEFERRED`. The remaining open findings are measured
outcomes/evidence limits rather than an unbuilt local code task, so a second
`speckit-converge` appends no replacement-run work.
