# Spec 131 Corrected Harness Preflight

## Frozen subject and harness identities

- Baseline base: `a9944019f76791773604999f00128057b9534ace`
- Baseline temporary build head: `b1985cd24c3c5514df72b9655d24bd5be3c88825`
- Baseline binary SHA-256: `79e53ab4f6e46fa00962e6f646b6374dfba341829d8aae67f8c7c9b15955b465`
- Baseline library SHA-256: `b0e1742084cffd207a929610014234b0dad678bf26edf08bd737457df2e02964`
- Latest base: `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Latest temporary build head: `088eeb43e2e643841c32a6deab1878a80a1469b5`
- Latest binary SHA-256: `0f042bcf622bd5229368c483d9f24ab0eb739d6500f0855f1306b760810628f9`
- Latest library SHA-256: `66cae840c4cc48195b1d8fa47fbf0ec46222f6ae0282295abd015a4e51491159`
- Canonical Boost patch SHA-256: `c983a7e9a39b9a01207f1810f6d9d84a360cb383d3a7016d59aeb0ff877fe3a4`
- Subject authority SHA-256: `7971e8c2a33eea0e7f567035dd51526cca56fdb1ab9aa79ac416d2e747c9f6a3`
- Runner SHA-256: `86914868e7b8969963773b3341489bc2716e64ad91510adb8941ebcdd29172a2`
- Analyzer SHA-256: `a2791d256262a2d0e5a90cb8a3a57e9e2c216946a1482d02050d9795b29d96d1`
- Driver SHA-256: `6f7fc921c531daf1747dfba563f6eae8672111dc724fc5f90d8a8aeecdb29c79`
- Build authority SHA-256: `4057305aa51544aad756dc45621c89e00075fc86a66ecbe3c5572f3d62dfe834`

Both worktrees were clean after their identical sole `wscript` patch. Both C++
self-tests passed. Dynamic linkage resolved Boost 1.71 and neither Boost 1.74
nor NDNSF. The focused Python suite passed 6/6 and strict Spec Kit structure
audit passed with 22 requirements traced to 7 tasks.

## 1000 pps admission smokes

| Subject | Scheduled | Attempted | API-completed | Delivered | Attempted pps | Attempted/scheduled | Delivered/attempted | Admission |
|:--|--:|--:|--:|--:|--:|--:|--:|:--|
| baseline-sync-serial | 5000 | 5000 | 5000 | 2049 | 1000.0 | 100.00% | 40.98% | PASS |
| latest-async-parallel | 5000 | 5000 | 5000 | 5000 | 1000.0 | 100.00% | 100.00% | PASS |

Raw non-formal evidence:

- `/tmp/spec131-pacer-baseline5-parent-vfyjx4/run`
- `/tmp/spec131-pacer-latest-parent-nAWEpH/run`

These smoke delivery observations are not formal results. Their only admission
purpose is to prove that each corrected harness reaches 1000 pps attempted rate
within +/-2%, with clean process exits and explicit loss accounting.

The earlier 50-cell `confirm01` remains immutable diagnostic evidence only, as
documented in `diagnostic-harness-failure.md`.
