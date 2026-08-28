# Spec 135 Post-Implementation Audit

**Date**: 2026-07-22  
**Verdict**: PASS / FROZEN

## Admissible campaign

- Campaign:
  `results/spec135-svs-fetcher-queue-causality/spec135-rsa-confirm02-20260723T051420Z`
- [x] Exactly eight once-only receipts exist and all are `COMPLETE`.
- [x] Exactly 16 peer rows prove TLV signature type 1
  (`SignatureSha256WithRsa`).
- [x] All 16 peer rows have complete 81-stage profiler output.
- [x] Each cell has two separate MiniNDN node homes, PIB databases, private-key
  directories, client configurations, and verified routes.
- [x] Five stage-A cells ran in the frozen 200/400/600/800/1000 order.
- [x] The 400-pps stage-A baseline was reused, not rerun; exactly three
  stage-B treatments ran.
- [x] No automatic retry, replacement, or selective omission occurred.

## Invalid harness history

- [x] `confirm01` is retained as `HARNESS_INVALID_ABORTED` with zero accepted
  receipts because raw `host.popen()` bypassed MiniNDN per-node application
  environments.
- [x] No partial `confirm01` data appears in the analyzer or final report.
- [x] The corrected runner uses MiniNDN `popenGetEnv()` semantics and the
  default per-node persistent RSA KeyChain.

## Findings and claim bounds

- [x] The preregistered 98% operational boundary is 400 pps/peer; the sharper
  attempted-rate knee is 600 pps/peer.
- [x] RSA inner plus outer signing is approximately 1.09--1.10 ms per
  publication and approximately 95% of `PUB.TOTAL`.
- [x] The frozen DigestSha256 baseline is used only as an explicitly labeled
  comparison and is not relabeled or rerun.
- [x] The Fetcher-window verdict is `PARTIAL`: both matched contrasts reduce
  Payload queue residence, but attempted-rate effects disagree.
- [x] The piggyback-capacity verdict is `PARTIAL`: both matched contrasts reduce
  fallback Interest count, but queue/pacing effects disagree.
- [x] Shared-I/O causality remains mechanism-supported rather than isolated.
- [x] Validators-disabled, software file-TPM RSA, one observation per cell, and
  the difference between an operational boundary and saturation are explicit.

## Integrity

- [x] `evidence-manifest.json` verifies 19 authoritative artifacts.
- [x] Structural Spec Kit audit passes: 15 FRs, 7 SCs, 4 stories, all traced.
- [x] Six source/contract tests pass.
- [x] `git diff --check` passes for all Spec 135 source, tests, and documents.
- [x] Production NDN-SVS, active NDN-SVS checkout, NDNSF, and Spec 133 formal
  campaign evidence remain outside the Spec 135 diagnostic patch.

No HIGH or CRITICAL finding remains. Spec 135 is closed and MUST NOT be rerun
or selectively supplemented.
