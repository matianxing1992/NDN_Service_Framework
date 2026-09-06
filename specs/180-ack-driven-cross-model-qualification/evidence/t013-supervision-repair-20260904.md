# T013 supervision repair — 2026-09-04

Verdict: **focused supervision repair PASS; full T014 remains BLOCK**.
Authority: revision 123 and the revision-112 single-YOLO scope. This continues
`t014-tiger-path-audit-20260904.md` without replacing its frozen observations.
HEAD remains `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`; dirty work is preserved.

## Implemented and wired

- `run-ndnsf-yolo.sh` is now a thin entry to sibling `supervise-tiger.py`.
  There is one node-local process supervisor, not parallel shell/Python owners.
- `supervise-tiger.py:execute` starts NFD, waits for its bounded probe, starts
  Controller and waits for `SPEC180_RUNTIME_CATALOGUE_PUBLISHED`, starts Repo
  and waits for installed permissions, then starts the four native Providers.
  All four must emit the post-install `NDNSF_DI_NATIVE_PROVIDER_READY provider=`
  marker; the earlier `SERVE_READY ... runtimeStatus=installing` cannot qualify.
  The publication receipt and monitored five-second SVS settle precede User.
- Every readiness wait checks all already-started children before accepting
  a marker, and again after its predicate. User completion also checks peers
  and has a 120-second hard bound; a printed success marker cannot bypass it.
- Each runtime child has a new process session/group and a distinct HOME.
  Cleanup broadcasts SIGINT, allows 15 seconds, escalates remaining groups to
  SIGKILL, and allows three more seconds. It checks groups even when the
  original leader exited. Return codes, forced/timed-out children, PIDs, live
  groups and closed log streams are recorded without normalizing signal exits
  into zero. A second SIGINT/SIGTERM cannot interrupt the cleanup section.
- `supervision.json` binds the observed records to candidate ID/digest.
  Terminal validation is called after runtime cleanup. It checks the existing
  result validator plus exact observed child records, Provider PIDs and cleanup
  fields. Missing `spec180-result.json` is `TERMINAL_RESULT_MISSING`, exit 8;
  there is no marker-only COMPLETED/PASS fallback.
- `run-functional.sh` now probes and executes launcher/renderer in the same
  `/opt/ndnsf-di/replay/repo/packaging/ndnsf-di-container/jobs/spec180` tree.
  Bootstrap is sibling-loaded from that tree. The submit bundle is no longer
  the executed launcher source. The source archive allowlist now includes
  `scripts/validate_spec180_results.py`; the existing jobs-directory selection
  includes the new supervisor. Qwen dispatch fails before file/container access.

## Prior finding dispositions

| Finding | Disposition / boundary |
|---|---|
| TP-01 startup ordering | Repaired in the production caller; phase/early-exit regressions pass. No live NFD/Tiger run performed. |
| TP-02 cleanup/result chain | Runtime-group cleanup and fail-closed terminal consumer repaired. Real oracle production, secret/scratch disposal and complete transient-helper supervision still open. |
| TP-03 probed/executed launcher mismatch | Source-level path mismatch repaired and source inclusion tested. New in-image build/closure remains unexecuted and blocked. |
| TP-04 live Y-N negatives | Unchanged; still open under T011. |
| TP-05 candidate-plane binding | New source-only checkpoint below; full dependency/runtime/SIF/design/harness binding remains open. |

## Focused execution evidence

Initial new test run: 2 failures and 7 setup errors because the replacement
supervisor did not yet exist and the bundle launcher path was still present.
After implementation, 16 targeted supervision/entrypoint cases pass. Tests use
real tiny Python subprocesses for process death, marker precedence, timeout,
SIGINT-resistant children, group cleanup after leader exit, and return-status
collection. Production `execute()` phase-order/failure paths use explicit
runtime/bootstrap doubles; they do not start NFD or mint identity keys.
The result-binding positive fixture is synthetic, not a measured GPU result.

Final focused command:

```bash
python3 -m pytest -q --tb=short \
  tests/python/test_spec180_tiger_supervision.py \
  tests/python/test_spec180_tiger_contract.py \
  tests/python/test_spec180_release_workflow.py \
  tests/python/test_prepare_local_sif_source.py
```

**44 passed in 2.48s**. Shell syntax and relevant `git diff --check` pass.
No full suite, network qualification, container build/replay, upload, SSH,
GPU allocation or scheduler submission occurred.

## Remaining controlling work

1. **T010/T007/T013 evidence production:** the maintained YOLO User currently
   records response status/digest and prints a marker (`user.py:273-293`), not
   the registered numerical comparison. Produce the numerical oracle from the
   real response using the existing YOLO adapter/reference contract. Produce
   actual native role/PID/CUDA EP/physical UUID records, not launch-argument
   claims. Assemble and validate the final candidate-bound result only from
   those records plus observed supervision. Do not fabricate successful fields
   merely to satisfy the newly connected validator.
2. **T013 full cleanup:** collect/bind transient bootstrap and `nfdc` helper
   outcomes and their descendant cleanup as well as the eight runtime children.
   Current helper calls have bounded direct waits but are not yet in the runtime
   group ledger. Complete run-owned secret/scratch disposal and redacted evidence
   policy; current private scratch is deliberately retained, not called clean.
   The descendant test proves no live group member remains, not that orphan
   zombie statuses were collected by the supervisor. This distinction must be
   resolved before claiming complete child closure.
3. **T011 / TP-04:** implement real critical Y-N negatives; focused substitutes
   must not become network qualification.
4. Bind all current candidate planes and rerun full T014. Only a fresh PASS
   permits T015, then a new SIF and Tiger. The terminal consumer intentionally
   fails until its actual evidence producer is implemented.

These are known implementation gaps, not requests for new experiments or a
new inference architecture. No task box is newly marked complete.

## Exact source-only checkpoint

Existing prepare/validate helpers generated and verified:

- `.codex-tmp/spec180-supervision-source-20260904-BGWnp2/source-seal.json`
- 408 source files, 15,790,080-byte `workspace.tar`, zero compiled payloads.
- Archive SHA-256: `sha256:f4d738a892aeeb79be67a73429bf15a6dfed02bcc25d7a8e5db2ac4bd4f6ecfb`
- Semantic seal digest: `sha256:8e045378930ab0e2dfc054db15e7ccd0aa8a46f55ba531691860bcd6f334ecf7`
- Source mode `sealed-current-worktree-files`; no dependency archives.

This replaces the prior 406-file snapshot only as the latest source checkpoint.
The old snapshot is preserved and cannot qualify this changed harness. Neither
snapshot is a complete CandidateRecord, built runtime, or permission to build.

| Changed file | SHA-256 |
|---|---|
| `packaging/ndnsf-di-container/jobs/spec180/supervise-tiger.py` | `6b6340942b79eedda9f64a7c276e54819e9eb690f8da98f8efef312ce1c0498d` |
| `packaging/ndnsf-di-container/jobs/spec180/run-ndnsf-yolo.sh` | `cd2daf1ef90f17d402ae831bc2339954c5dac5441c75053f2d569484743d5679` |
| `packaging/ndnsf-di-container/jobs/spec180/run-functional.sh` | `3c1ae8f50956b3ecf28f38e7d26a807363825c58e5d43dfd274f87e61e53e7c1` |
| `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/prepare-local-sif-source.py` | `7f978eabbe36e1009126d3bfa7553e17afdf65c7795216edbf5783ebf7add4fa` |
| `tests/python/test_spec180_tiger_supervision.py` | `85051eeda14d3e4160f23d43fec1be51876694d60be2d13a0a4a4d5ce19201ba` |

## Workflow gates

Context Mode project/active health passed at entry; repository authority was
used. CodeGraph was consulted first; archived duplicate symbols required exact
canonical file reads. Spec Kit requirements checklist is 25/25; implementation
and code-aware audit rules keep T014 blocked. GSD installation health passed,
but its phase-36/Spec170 checkpoint is stale for this request, so no unrelated
phase was resumed. ARS is not applicable to implementation-only work. No
DeepSeek configuration, credentials, unrelated worktree edits or git history
were changed.
