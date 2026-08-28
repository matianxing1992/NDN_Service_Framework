# Spec 135 Pre-Implementation Audit

**Date**: 2026-07-22  
**Verdict**: PASS

## Intent and evidence boundary

- [x] Spec 133 remains frozen evidence. Its already measured Data signer is
  explicitly identified as `DigestSha256`, not RSA.
- [x] Spec 135 owns a new result directory, subject manifest, patch, binaries,
  manifests, receipts, and report.
- [x] The requested RSA correction is implemented as a fresh rate sweep rather
  than relabeling or selectively supplementing Spec 133.

## Source reality and necessity

- [x] The frozen profiled parent is
  `e9913c9a957a214d699ab5eb0bc99684e06573c5`.
- [x] `Fetcher` admits at most `m_windowSize = 10` pending Interests.
- [x] `updateCallbackInternal()` expands missing sequence ranges into individual
  fetch work and `fetchAll()` scans pending keys.
- [x] A window intervention is therefore local, necessary, and falsifiable.
- [x] The 4096/7168-byte intervention is bounded below the configured 8800-byte
  face MTU, but fragmentation remains an explicit alternative explanation.

## RSA contract

- [x] Each peer creates one `RsaKeyParams(2048)` identity before warmup.
- [x] `security::signingByIdentity(identity)` configures the exact
  `KeyChainSigner` subsequently passed to `SVSPubSub`.
- [x] A probe signed through that signer must expose TLV type
  `SignatureSha256WithRsa`; a mismatch terminates the peer before measurement.
- [x] Identity/key generation and the probe are excluded from measured stage
  durations.

## Experimental validity

- [x] Stage A has exactly five once-only RSA cells: 200, 400, 600, 800, and
  1000 pps/peer.
- [x] The boundary selection rule is declared before execution.
- [x] Stage B adds exactly three treatments at the selected rate; its stage-A
  `W10-P4096` baseline is reused and not rerun.
- [x] Two processes/nodes, verified dual-prefix routes, one Face/io_context
  thread per peer, fixed CPUs, 10/60/10 timing, 256-byte payload, and zero
  configured loss remain controlled.
- [x] One observation per cell is labeled descriptive and cannot support
  confidence intervals or population inference.

## Security, migration, and rollback

- [x] The diagnostic worktree is never installed, merged, rebased, or pushed.
- [x] HMAC Sync Interests and disabled validators are held fixed; only the Data
  signer intentionally changes from Spec 133.
- [x] No NDNSF or production NDN-SVS runtime is modified.
- [x] Rollback is removal of the isolated worktree/build only after evidence is
  frozen; unfavorable results are retained.

## Tool and readiness gates

- [x] Context Mode: repository instructions, constitution, Spec 133 evidence,
  and active Spec 135 artifacts were reviewed.
- [x] CodeGraph was used before source-impact analysis; ignored historical
  worktree paths were then inspected directly.
- [x] Spec Kit structural audit: PASS (15 FRs, 7 SCs, 4 stories, all traced).
- [x] GSD installation health: PASS.
- [x] ARS experiment-agent contract is recorded in `research.md`.
- [x] The repository's requested agent-context update script is absent; this is
  recorded as a tooling limitation and does not alter experiment semantics.

No HIGH or CRITICAL finding remains. Implementation may begin.
