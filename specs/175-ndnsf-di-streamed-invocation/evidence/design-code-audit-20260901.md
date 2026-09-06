# Spec175 fresh design-to-code convergence audit — 2026-09-01

> **Historical/superseded report.** The later canonical transport repair changed
> the production publication/Selection path and invalidated the source seal and
> T020/T022 manifests described below. The current implementation verdict and
> release sequence are recorded in
> [`design-code-audit-20260901-canonical-transport.md`](design-code-audit-20260901-canonical-transport.md).

## Verdict

**PASS for implementation convergence.** The repaired current `Experimental`
source satisfies the controlling Spec175 production-path contracts at the
focused implementation boundary.  This PASS does not by itself claim exact
SIF G4, CUDA, TigerCluster, or performance qualification; those are separate
release gates.

## Subject and authority

- Feature: `specs/175-ndnsf-di-streamed-invocation`
- Branch: `Experimental`
- Authority: current `spec.md`, `plan.md`, `tasks.md`, versioned contracts,
  and the five-item convergence checklist in
  `.specify/memory/design-code-convergence.md`
- Source was inspected through CodeGraph and exact on-disk source checks.
- The worktree remains intentionally dirty outside the already sealed subject;
  no *new* source-bound candidate may be claimed until those inputs are frozen,
  but this does not invalidate the implementation convergence result or the
  existing T020/T022 manifests.

## Controlling path results

| Contract surface | Current owner | Result | Focused evidence |
|---|---|---|---|
| Ordinary streamed V3 boundary | `AutomaticPlanningCoordinator` and `validate_spec175_ordinary_v3_proposal` | PASS; V2, hybrid, non-zero-rank, tensor degree >1, incomplete roles, and duplicate Provider ownership are rejected before Selection | `tests/python/test_spec175_v3_boundary.py`; `tests/python/test_spec175_contract_gate.py` |
| Post-Selection Provider assembly | `DI_NativeProviderExecutable`, `NativeProviderHandler`, `NativeCanonicalOnnxAssembler` | PASS; formal path installs a preparation factory, binds the assigned canonical root, fetches/validates source, assembles and signs the role, and disables preassembled compatibility | `evidence/t038-native-assembly-helper-20260901.md`; native assembly integration cases |
| Native sampling/text terminal | `NativeEpochCoordinator` and standalone tokenizer | PASS at implementation boundary; authenticated sampling, Unicode/split-stop deltas, EOS/stop terminal text, and digest-only/empty-delta mutations are covered | `evidence/t039-native-terminal-20260901.md`; `tests/python/test_spec175_native_oracle.py` |
| Request-local device state | `NativeProviderRuntime`, `ProviderRoleWorker`, ONNX adapter | PASS at implementation boundary; coordinator receives an opaque adapter handle and rejects missing handles rather than serializing complete state | `evidence/t040-opaque-state-handle-20260901.md` |
| Conversation state transfer | `ConversationStateStore` and `OnnxRuntimeModelRunner` | PASS at implementation boundary; adapter owns promotion, restore, D2H pause, H2D prefetch, cancellation fence, release, and byte accounting; CPU keeps the explicit host fallback | `evidence/t031-adapter-state-transfer-20260901.md` |
| Runtime diagnostics and pre-dispatch gates | `ndnsf.di.RuntimeEvidence`, candidate closure and route/terminal validators | PASS at implementation boundary; named severity filtering, bounded/privacy-safe records, and zero-side-effect rejection are covered | `evidence/t041-logging-20260901.md`; `evidence/t035-candidate-closure-20260901.md`; `evidence/t036-route-terminal-20260901.md` |

## Focused checks

The current repair set passed:

```text
./waf build -j1 --targets=unit-tests                                  PASS
./build/unit-tests --run_test=NativeProviderRuntimeUsesAdapterOwnedConversationTransfers PASS
./build/unit-tests --run_test='*ConversationState*'                   PASS (6 cases)
python3 -m pytest -q tests/python/test_spec175_contract_gate.py        PASS (15 cases)
python3 -m pytest -q tests/python/test_spec175_native_oracle.py         PASS (3 cases)
python3 -m pytest -q tests/python/test_spec175_native_assembly.py       PASS (3 cases)
python3 -m pytest -q tests/python/test_spec175_sif_preflight.py \
  tests/python/test_spec175_real_minindn_gate.py                        PASS (94 cases)
```

The ORT-enabled unit target compiled and linked all 104 current targets. The
checks above are task-local repair evidence; they are not a substitute for a
fresh source seal or the later environment-specific gates.

## Five-item convergence checklist

1. **Design/configuration frozen:** PASS for the current Spec175 documents and
   ordinary-V3/ONNX-only/default-disabled-replacement boundary.
2. **Production path traced:** PASS from Python public request through ACK
   closure, V3 planning/Selection, native Provider preparation, role execution,
   state ownership, terminal output, and evidence sinks.
3. **Discrepancies closed:** PASS for F01--F08 from the 2026-08-31 audit; each
   has a current owner, focused correction, and regression. F09 is addressed by
   the strengthened gate and evidence separation.
4. **Fresh repaired audit:** PASS, this report, for the current source subject.
5. **Exact qualification identity:** the current T020 source seal and T022
   42-process G3 manifest now bind the executable, libraries, Python runtime,
   artifacts, cwd, arguments, environment, and topology.  Exact-SIF G4 remains
   the next unpassed gate and must bind the same identity before promotion.

After the audit wording was synchronized with the hard-gate regression, the
current build also passed the complete native unit binary (`exit 0; no errors
detected`) and the combined current Spec175 contract/oracle/assembly/preflight
suite (`115 passed`). These are regression confirmation for the repaired
subject, not a source-sealed G0--G3 manifest.

The repository contract gate was then run against the real worktree and
returned the expected fail-closed `BLOCKED` result: `DIRTY_INPUT_TREE`, 143
in-scope paths, with no network, staging, or scheduler side effect. This would
block any *new* candidate seal; the existing T020/T022 source-bound manifests
remain the current local qualification subject, and the remaining release
blocker is T023's exact-SIF canonical-object fetch.

## Remaining work

T042 is closed by this report.  T020 and T022 subsequently passed for the
current source seal and strict 42-process host/CPU matrix.  The six remaining
tasks are qualification and closure only: T023 (exact-SIF G4, currently
blocked by the canonical-object fetch), T025/T026/T034 (Qwen/CUDA/device-tier
evidence), T027 (registered performance characterization), and T028 (final
traceability).  No old manifest, SIF, or Tiger result is promoted across the
repaired source identity.
