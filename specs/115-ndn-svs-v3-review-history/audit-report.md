# Spec 115 Post-Implementation Audit

## 2026-08-07 Focused-PR Override

`PASS` for the current local objective. The superseding audit boundary is the
three-commit `pr/svs-v3-focused@5db5e6a` branch within the complete reorganized
local `master@3c96ab4` history. The focused branch
contains only code, direct tests, examples, and deterministic fixtures for
regex/name-only PubSub, V2 InterestSigner, and corrected SVS V3. The descendant
contains Mapping/piggyback, parallel processing/production, sparse recovery,
segmented publication atomicity, and bounded Fetcher/Repair.

Exact focused units pass 40/40, exact full-master units pass 74/74, and the
NDNSF-owned standalone C++/TypeScript matrix against the focused branch passes
5/5. The original dirty `Experimental@6bb3454` checkout and its two dirty-state
hashes remain unchanged. `origin/master`, `upstream/master`, tags, and PR
metadata were not mutated. Full identities and commands are in
`evidence/focused-v3-rewrite-20260807.md`.

On explicit user request, the focused branch was subsequently published as
`origin/pr/svs-v3-focused@5db5e6a`. `origin/master`, tags, and PR metadata remain
unchanged; no pull request has been created yet.

The earlier nine-commit publication audit below is retained as historical
evidence; where it conflicts with this override, FR-033-FR-040 and T039-T043
control. Publication remains intentionally user-controlled.

## Verdict

`PASS` for local nine-commit ownership, separate producer/recovery state-machine
owners, permanent NDN-SVS Boost 1.74 policy,
disposable `compileTMP` Boost 1.71 validation, standalone interoperability, and
immutable MiniNDN validation.

`BLOCK` for complete Spec 115 closure because the exact corrected head is not
published and therefore cannot have green exact-head GitHub Actions. This is an
explicit user-controlled publication hold, not an unresolved local code defect.

## Findings

| ID | Severity | Dimension | Finding | Required action |
|---|---|---|---|---|
| A115-01 | HIGH | Published CI | Published head `0c09d65` fails 6/31 Ubuntu 22.04 jobs because `fetcher.hpp` lacks a direct `<atomic>` include. | Do not claim the published head is green. Publish corrected candidate only after renewed authorization. |
| A115-02 | RESOLVED LOCALLY | Code/ownership | The old combined segmented-publication/recovery owner contained two independent state machines. | Split producer reservation/staging/commit/rollback into `79d830a` and receiver fetch/validation/Repair into `ee4e174`; the final tree is unchanged. |
| A115-03 | RESOLVED LOCALLY | Validation | The nine rewritten OIDs required fresh local evidence and could not depend on descendant APIs or logger setup. | All nine exact OIDs pass complete available units through created-and-deleted `compileTMP` branches; test/API migrations were repaired in their semantic owners. |
| A115-04 | BLOCKING AUTHORITY | Publication | User explicitly prohibited further push until a new request. Remote/PR remain on `0c09d65`; local master/Experimental are `ee4e174`. | Wait for explicit push instruction, then refence, back up, lease-publish, and monitor exact-head CI. |

## Intent and ownership audit

- The candidate contains exactly nine behavior-owned commits; the committed
  Boost 1.71 build-policy owner is removed.
- `setPeriodicSyncTime()` is owned by parallel Sync commit `cfc6270`, not the
  bounded-piggyback owner `3b709b4`.
- Producer publication state and receiver recovery state have separate owners.
- Official V3 wire behavior remains separate from fork-only Mapping/Repair
  policy, and every tracked NDN-SVS commit retains Boost 1.74.
- The final runtime tree equals frozen source candidate `a965ad6`; no
  protocol or runtime behavior changed during the boundary correction.

## Verification reality

| Claim | Evidence | Status |
|---|---|---|
| old-compiler CI failure root cause | GitHub job logs for run `29545641123` | verified: missing `<atomic>` |
| state-machine ownership | producer/recovery source split and nine-commit sequence | verified |
| all reviewed commits retain Boost 1.74 | per-OID `wscript` scan | verified, 9/9 |
| every exact OID compiles on host Boost 1.71 | `evidence/nine-owner-validation.md` and disposable logs | executed, 9/9 PASS |
| final temporary branch cleanup | `git branch --list compileTMP` | verified, zero |
| standalone NDN-SVS/NDNts interoperability | five-case TypeScript/C++ matrix | executed, 5/5 PASS |
| MiniNDN 0%/5% interoperability | candidate `spec114-a049bf032f73fc62e192` | executed once, 6/6 PASS |
| exact corrected head has green GitHub CI | no publication allowed | not available |

The formal MiniNDN matrix produced 240/240 bidirectional observations, equal
final vectors in all six cells, and zero duplicates, rejects, restarts, or
Sync-Ack packets. Aggregate-only verification passed without rerunning cells.

## Remote and review audit

- Remote master and PR #36 head: `0c09d65`.
- Corrected local master and Experimental: `ee4e174`.
- Remote safety branch retains pre-publication head `2b052c9`.
- Local safety ref retains published head `0c09d65`.
- PR description was rewritten in plain language and the four stale inline
  review threads were answered and resolved before the later no-push hold.
- Historical `reviewDecision=CHANGES_REQUESTED` is not represented as approval.
- No upstream branch, tag, or release was changed.

## Metrics

- Functional requirements: 32
- Success criteria: 14
- Tasks: 36/38 complete; T033 and T034 open
- Surviving commits: 9
- Tracked NDN-SVS Boost variants: Boost 1.74 only
- Disposable `compileTMP` Boost 1.71 unit gates: 9/9 PASS
- Standalone cells: 5/5
- MiniNDN formal cells: 6/6
- Critical / High / Medium / Low findings: 0 / 1 / 0 / 0

## Required next action

Do not push. Preserve `ee4e174`, the immutable MiniNDN candidate, and all
safety refs. Resume T033 only after the user explicitly requests a push. Then
use a newly refreshed exact lease; never reuse the earlier `2b052c9 -> 0c09d65`
publication command for the corrected candidate.
