# T011 / S2c — Y-N Critical Controls (Revision 116) — 2026-09-04

> **INVALIDATED — DO NOT REUSE.** Revision 123 changed the Controller
> PUBPARAMS readiness/runtime source after this control run (spec181 R003
> banner audit).  The current-source Y-N wiring records are the
> `t011-y-n-live-current-20260905-r35`…`-r42` series.  This historical
> control PASS is retained for audit history only and cannot satisfy a new
> Y-N gate.

**Status**: `SPEC180_CASE_RESULT status=PASS case=Y-N` (runner exit 0).

## Setup

- Case bundle: `.codex-tmp/spec180-yolo-y-n-inputs-r115/` — exactly four
  identities with a distinct shared-role cover plus one FullModel
  capability:
  `/example/provider/BackboneNeck` advertises `FullModel, BackboneNeck`;
  `DetectShard0`, `DetectShard1`, `Merge` each advertise one role.
  Providers are spread across ucla/neu/arizona/wustl.
- The runtime publication carries BOTH candidate snapshots
  (`atomic-v1` 74204676… and `shared-backbone-two-shard-v1` 57547ffc…)
  from the same signed package manifest.

## Observed controls

- **Y-N-O order invariance (CONTROL)**: four ACKs closed (ackCount=4); both
  candidates are feasible; the planner applied the signed per-candidate
  priority (shared=20 > atomic=10) and selected
  `shared-backbone-two-shard-v1`; the four-role certified-subgraph pipeline
  executed and produced terminal result digest
  `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
  — byte-identical to Y-A, Y-B, and the offline full-model forward.
- **Signature control**: every V3 offer verified against the registered
  candidate-bound trust root and the Trust-Schema-validated ACK Data
  (live, not fixture HMAC).
- **Digest control**: candidate digest, graph identity, manifest digest,
  and per-role recipe digests bound from the signed catalogue through the
  sealed plan; the published snapshot must match the adapter runtime
  digests (a mismatch forces fail-closed rejection — regression-tested).
- **Role-owner control**: one-to-one role ownership (four roles on four
  Providers, distinct cover); the dual-capability Provider advertised
  `FullModel` + `BackboneNeck` and was selected for exactly one role.
- **Cleanup control**: the runner teardown closed all four Provider
  processes, the Repo, the Controller/User, and the MiniNDN network with
  no leftover NFD/Provider processes.
- The FAIL_CLOSED expectations for the negative subcases (Y-N-C/P/R/I/E/L)
  are declared in the case plan (`case-plan.json` subcases) and are
  additionally covered by the focused suites: feasibility/infeasibility
  (test_spec180_yolo_ack_planning), offer signature/provenance
  (test_spec180_provider_offer_trust, test_spec180_ack_provenance),
  replay/security (test_spec180_yolo_security), role ownership and
  assembly (test_spec180_role_assembly).

## Code changes this revision (each with a focused test)

- `prepare_spec180_yolo_case.py` supports Y-N (four-identity cover
  contract, spread nodes).
- The runtime catalogue encode/parse uniqueness rule allows multiple
  candidates from one signed manifest (`(manifest, candidate)` pairs are
  unique; a single manifest digest per candidate no longer required).
  `app_sdk/placement.py`; focused resolver/application tests pass.

## Boundary

Local MiniNDN Y-N control evidence. T014 audit, T015 inventory, SIF, and
Tiger remain.
