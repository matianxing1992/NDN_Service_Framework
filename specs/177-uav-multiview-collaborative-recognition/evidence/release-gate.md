# Spec 177 release gate (working-tree evidence)

**Gate date**: 2026-08-29  
**Branch**: `UAV-Experimental`  
**Feature tasks**: 13/13 complete

## Passed locally

- Python contract/model/output/security/evaluation tests: `18 passed`.
- C++ multi-view unit tests: `4/4` passed.
- C++ multi-view CPU integration tests, including coordinator terminal
  acceptance: `3/3` passed.
- Generated six-view fixture: all 1/2/4/6-view functional runs completed;
  hashes and six annotations were checked.
- Controlled registration/evaluator: completed with point metrics and the
  explicit `scientificAccuracyClaimAllowed=false` marker.
- Real MiniNDN matrix: five scenarios passed under `sudo -n`; nominal and
  Provider-selection completed with six verified image Data packets, six
  annotations, one result owner, and two eligible Providers.  Unavailable,
  late, and publication-failure scenarios reached explicit rejected terminal
  statuses.  Result hashes and event counts are in
  `evidence/minindn-matrix-20260829.md`.
- MiniNDN contract tests: `5 passed`; unprivileged invocations remain
  fail-closed and cannot fabricate transport evidence.

## Not claimed

- The generated car fixture and one-sample controlled campaign provide
  functional point estimates only; they do not establish recognition
  accuracy, mobility gains, or statistical significance.  The evaluator marks
  paired-bootstrap uncertainty as `insufficient-samples`.

## Verdict

**PASS for the functional Spec177 scope.**  No Spec177-specific BLOCK finding
remains.  A future research extension should replace the synthetic one-sample
registration with a licensed calibrated multi-sample dataset before making any
accuracy or multi-view-benefit claim.
