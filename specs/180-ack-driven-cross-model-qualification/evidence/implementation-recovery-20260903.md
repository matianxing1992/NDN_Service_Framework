# Spec180 implementation-recovery checkpoint (revision 102)

**Date**: 2026-09-03

**Verdict**: `BLOCKED_BEFORE_EXECUTION`

This checkpoint explains the delay without treating a missing experiment as a
negative NDNSF-DI result.

## What caused the delay

1. The original `run_minindn_case()` stopped after side-effect-free validation
   with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. Contract and seam tests could
   pass, but no NFD/SVS process or request was created.
2. The first barriered driver has now been added, but the temporary YOLO set
   is not a sealed release input: most packages use obsolete `DetectHead0/1`
   role names, while the one observed package with `DetectShard0/1` has no
   trusted catalogue signature. The current exporter and adapter require the
   current names plus a registered signature; editing a manifest in place
   would break its candidate/graph digest binding.
3. The current Provider process vector now validates and passes two
   candidate-bound runtime inputs: the Ed25519 private key used to sign
   `ProviderOfferV3` ACK metadata and the canonical local ONNX path for the
   atomic `FullModel` case. The required external files are still absent from
   one sealed candidate manifest. Without them, the User's trust verifier or
   the Provider execution path cannot qualify a real request.
4. SIF/Tiger work was intentionally held behind this local vertical slice.
   Building or submitting an earlier candidate would either repeat a
   pre-network input failure or produce evidence for a source/runtime pair
   that was not sealed together.

5. A controller-entry defect was found during the production-path recheck:
   the runner supplied a Spec180 publication file but no repository deployment
   manifest, while the Controller only entered its publication branch when the
   latter was present. The Controller therefore never published the signed
   catalogue/artifact batch and the runner could not pass its receipt barrier.
   The branch is now corrected and covered by a focused regression; this was a
   wiring defect, not an NDNSF-DI protocol result.

6. The corrected Controller now validates the cross-process publication
   envelope before signing: case and signer namespace, package/catalogue
   digests, unique artifact names, bounded payloads, and per-artifact digests.
   A focused tamper test passes. This prevents a mutated publication file from
   becoming apparently valid evidence, but it does not supply a live protocol
   result.

7. The maintained YOLO User now instantiates `LifecycleJournal`, binds the
   coordinator request/attempt identity before input publication, receives the
   seven planning transition callbacks, appends the input/Provider/terminal
   milestones, and validates the ten-event trace. This closes source wiring
   only; no live trace or terminal Response exists yet.

8. The task graph is now explicitly bounded to G0 input closure, G1 atomic
   Y-A, G2 shared/negative cases plus T013, and one T014 audit. This prevents
   repeated preflight/audit loops from being mistaken for experiment progress
   and prevents SIF/Tiger work from consuming an unsealed candidate.

## Evidence recorded in this checkpoint

- The complete Spec180 Python collection plus the automatic-planning lifecycle
  and process-input digest regressions: 194 tests pass (22 existing warnings).
- `python3 -m py_compile` passes for the maintained coordinator, client,
  runner, and YOLO User.
- `scripts/spec180_contract_gate.py`: `status=PASS`,
  `qualificationReady=false`.
- `specs/175-ndnsf-di-streamed-invocation/`: remains a sealed
  `LOCAL_FUNCTIONAL_PASS` at its named source identity; its current-tree
  `DIRTY_INPUT_TREE` block is not reopened or used as Spec180 evidence.

These are implementation and contract checks only. No YOLO Y-A/Y-B/Y-N
terminal Response, complete local qualification, SIF replay, CUDA result, or
Tiger result is claimed.

## Required next action (revision 102)

Regenerate and validate one current YOLO package, supply the Provider
signing-key and local-model files through one candidate-bound manifest, run one
Y-A developer MiniNDN request, and preserve its request/attempt, ACK,
Selection, Provider, Response, child-exit, and cleanup evidence. Only after that result
passes may the same driver be extended to Y-B/Y-N and then enter T014.
