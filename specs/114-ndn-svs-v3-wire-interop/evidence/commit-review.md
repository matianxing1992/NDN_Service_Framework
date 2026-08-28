# NDN-SVS commit review

Date: 2026-07-16  
Base: `c34c04d766836bba1567a70bae846dfbd9d25b66`  
Head: `53dd1588201b967a4aa9decd3e51ade3263e0f88`

| Commit | Scope | Verification / reverse-order rollback |
|---|---|---|
| `76bb042` | explicit V2/V3 codecs and V3 Core profile | fixed vectors; foundational, revert last |
| `dd092ae` | validation, malformed/state safety | full unit suite; tests depend on codec |
| `ee2ea87` | bounded Mapping/Repair extensions | PubSub/Spec 113 regressions |
| `3636b32` | independent fixtures, peers, harness, docs | standalone and MiniNDN evidence tooling |
| `53dd158` | validator-before-semantic-decode, collection atomicity, deterministic face selection | 67/67 units, standalone 5/5, final 6/6 matrix |

The stack is linear; `git diff --check c34c04d..53dd158` passes. The target
worktree is clean. Final source tree is
`be3d9ccd348377160ab7afffabcbdea9459cff4f`; built library is `9988a7e3...ab92`.

## Test attribution

- standalone independent peer: 5/5;
- final MiniNDN candidate `spec114-9116d37e2e705bf281f4`: 6/6;
- final consumer candidate `spec112-7f67052175cf629158ab`: 8/8;
- detailed unit and focused counts are in `focused-validation.md`.

## Branch disposition

- `master` and `origin/master`:
  `db9fc25a5d5c27a44506da343823ef92d085a9c2`
- `Experimental`: `53dd1588201b967a4aa9decd3e51ade3263e0f88`
- relationship: Experimental is 9 commits ahead, 0 behind origin/master
- safety ref: `safety/spec114-pre-v3` -> `c34c04d...`
- merge/push/tag/release: not performed

Explicit V2 remains the isolated rollback profile. A human merge review can
revert Spec 114 in reverse order without moving master during this spec.
