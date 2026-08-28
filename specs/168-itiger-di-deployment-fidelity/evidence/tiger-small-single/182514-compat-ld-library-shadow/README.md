# Job 182514: compatibility child shadowed the candidate native core

- Candidate: `20260804T162714Z-v86-layer-complete-canary`
- Source identity: `sha256:a715983a037536cdb75bba56b27f4a695171ce427100852b4337db5da18212a1`
- Source bundle: `sha256:ca49455c118f30c420a9b3205981e5ab74df58c6f349ee52e0eda39aac1a09dc`
- Campaign: `spec168-campaign-v3-c155f65c8d4a71545753`
- Slurm job: `182514`, one admitted submission, no retry
- Slurm result: `FAILED`, elapsed `00:03:30`, rank step `00:01:32`

## Narrowest failure boundary

Model-free batch admission, all three outer wrappers, SIF launch, native overlay
ABI checks, the full reciprocal route mesh, and NFD readiness passed.  Rank 0
then failed while building generated policy, before controller/provider/user
startup or any Request.  The new `_ndnsf` extension reported an undefined
`ServiceUser::BeginCollaboration` symbol.

The entrypoint had correctly loaded candidate core
`sha256:50c059bef41a...`, but the reused Spec 162 compatibility kernel later
prepended `/opt/ndnsf-app/lib` to `LD_LIBRARY_PATH`.  A fresh policy-builder
Python process therefore loaded the old SIF core beside the new extension.
This is runtime native-identity drift, not a policy, route, cluster, model, Repo,
GPU, or NDNSF protocol failure.

## Repair and prevention

The compatibility kernel now preserves an inherited candidate-native library
directory ahead of the installed SIF library.  Immediately after the kernel's
final `LD_LIBRARY_PATH` assignment, a fresh child process imports `_ndnsf`,
checks `/proc/self/maps`, hashes the mapped core and extension, and emits
`SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS`.  Both canary and model analyzers require
exactly one matching marker per rank.  This extends admission from entrypoint
identity to actual child-process identity and directly prevents the same class
of regression as jobs 182511 and 182514.

Job 182514 remains immutable.  Requalification requires a new candidate and
campaign; this failed campaign is never resubmitted.

