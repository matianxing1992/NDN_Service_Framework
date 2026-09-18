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
| test/harness/oracle | `gap` | no Spec189 C++ placement selector/target exists yet |
| build/source closure | `gap` | no registered Spec189 oracle source/target map |
| migration/evidence | `covered-partial` | r04-r21 logs retained; acceptance and repeat remain open |

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: register and run the C++ request/placement
oracle, then preserve the no-fetch-before-Selection result. Existing ACK and
Selection observations do not count as task completion.
