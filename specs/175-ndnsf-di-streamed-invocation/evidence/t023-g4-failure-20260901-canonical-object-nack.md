# T023 G4 Failure Checkpoint: Canonical Object Fetch

**Date**: 2026-09-01  
**Candidate**: `v5-absolute-replay-20260901`  
**SIF**: `sha256:dbf6bf487ec28025adbce248dba5aed7bd1160265fe6e8b35a2d7726524c3194`  
**Source seal**: `sha256:e5b537bd219aac3b41fa350a90400250e455830008747cfe84b0ad6c4d738948`  
**Status**: `G4 BLOCKED`; candidate is not promotable and was not submitted to Tiger.

## What passed

The candidate build record and in-SIF preflight both pass with Apptainer 1.5.3.
The embedded CPython 3.10 extension, NDNSF/NDN modules, ONNX Runtime providers,
library closure, source-seal label, and forbidden-runtime checks passed. In the
host-orchestrated M01 replay, Controller, repository, User, and all four
Providers started; the route probe passed; all four ACKs were received and
matched; Selection was published and accepted by every Provider; and ordinary
Provider Responses were produced.

## First failing boundary

The first required canonical model-object fetch after Selection failed. Each
stage Provider reported `nacabe.Consumer: Data fetch error: Nack Error`. The
User then failed the collaboration assignment with `code=17` at the canonical
object name:

```text
/ndnsf-di/NDNSF-DI/MODEL/v1/NAME/NDNSF/Spec175TinyCausalLM/...
/OBJECT/sha256:121573cfaa1057d0402a5cfc4cc44f4ba28cb2a3f9865f5d0f7a32ef3bf59cdf/0
```

The retained run is
`results/spec175/g4/exact-current-v5-absolute-replay-20260901/M01-r1/`.

## Root-cause hypothesis requiring repair

The V3 projection asks the Provider to fetch a canonical `/ndnsf-di/...`
object, but the Spec175 tiny bootstrap publishes stage bytes only under
repository transport names such as
`/example/llm-pipeline/repo/NDNSF-ARTIFACT/sha256/<digest>` and advertises
`/example/llm-pipeline/tiny/segments/<digest>` in the active catalog. Neither
path is the canonical object Data name required by FR-079, and the bootstrap
does not install a canonical alias/ensurer. The resulting Nack is surfaced as
an authorization/decryption failure, so the current route probe does not catch
it. The missing `NDNSF_CONFIG` file was also corrected in a diagnostic replay;
the same canonical-object Nack remained, so configuration parity is not the
sufficient cause.

This is an implementation/evidence gap, not a reason to weaken canonical
names, bypass authorization, or reuse a historical host result. Before a new
candidate is built, the owner must either publish the signed canonical source
object and its role objects under the exact names selected by V3, or change the
trusted catalog/ensurer contract so the selected transport name is the
verified, reachable object identity. The choice must be covered by a focused
publication/route/decryption regression and a clean-room G4 case.

## Next gate

Keep T023 open. Repair the canonical publication/transport binding and extend
the G4 readiness probe to fetch one representative canonical object under the
same identity. Then regenerate the source seal, rerun G0--G3, build one new SIF,
and replay G4. Do not change timeout parameters or submit Tiger work to mask
this failure.
