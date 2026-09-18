# B189-2 Placement Evidence

**Status**: EXPLORATORY_PARTIAL / NOT_ACCEPTED

Real MiniNDN runs r04-r21 reached `ACK_CLOSED` and committed a signed
two-provider Selection. The requester logs show `Runtime.open → User.prepare →
PreparedModel.request`, and r21 records two `NDNSF_COLLAB_ASSIGNMENT_SELECTED`
entries followed by `NDNSF_DI_NATIVE_SELECTION_COMMITTED`. This is exploratory
runtime evidence only: the named C++ placement oracle has not run, the
request-envelope payload-free assertion has not run, and no-fetch-before-
Selection counters have not run. It therefore does not close B189-2.

The required acceptance order remains
`REQUEST_REFERENCE_ONLY → ACK → SELECTION_2_PROVIDERS`, with no layer fetch or
runner creation before Selection.

## Five-lane coverage

| Lane | State | Evidence / gap |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester and Core ACK/Selection path observed in r21 |
| implementation/wire | `covered-partial` | signed assignments and Selection marker observed; payload parser and placement binding oracle absent |
| test/harness/oracle | `covered-partial` | `spec189-request-wire` now proves the reference-only envelope and negative wire cases; ACK/Selection and no-fetch oracle remain absent |
| build/source closure | `covered-partial` | `tests/wscript` registers the wire-only selector; complete placement source map remains open |
| migration/evidence | `covered-partial` | r04-r21 logs retained; acceptance and repeat remain open |

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: register and run the C++ request/placement
oracle, then preserve the no-fetch-before-Selection result. Existing ACK and
Selection observations do not count as task completion.

## T004 wire-only request gate — 2026-09-18 11:35 -0500

The new C++ target `spec189-request-wire` calls the production
`encodeNativeRequestEnvelope` and passed three cases:

| Case | Result |
| --- | --- |
| repeated request envelope | stable model reference with all namespace/name/digest/size/epoch/scope fields; no model payload or URL; request and invocation identities differ |
| arbitrary URL / invalid reference | rejected before encoding |
| oversized inline payload | 16 MiB limit rejected before wire construction |

Build used the existing globally configured tree with `-j4` and completed in
16.778 seconds; logs are
`.codex-tmp/spec189-t004-request-wire-build.log` and
`.codex-tmp/spec189-t004-request-wire-selector.log`. The frozen static review
snapshot `.codex-tmp/spec189-t004-request-review-r2/diff.patch` (SHA-256
`8a074fa991d7aa35a7dc2ed64aa2510162c8f8863cdf8d483c3e946d7a0e7ef0`) received
`STATIC_PASS` with no P0/P1/P2.

This is deliberately a **wire-only** C++ gate. It does not exercise
`PreparedModel::request`, a prepared lease, Repo publication counters, or the
real two-provider ACK/Selection path. T004 is therefore `PARTIAL`, and B189-2
remains `NOT_ACCEPTED` until the production-handle and placement evidence run.
