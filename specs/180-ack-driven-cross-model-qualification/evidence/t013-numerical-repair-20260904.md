# T010/T013 registered input and numerical evidence repair

Date: 2026-09-04. Scope: focused implementation repair, not qualification.
Overall verdict: **T014 BLOCK**. No task box is newly marked complete.
Continues `t013-supervision-repair-20260904.md`; preserves all prior reports.

## Root cause and production repair

The ACK-driven User sent deterministic random `make_input()` data by default,
while the packaged oracle describes the registered fixed PPM fixture. It also
treated successful protocol response status as numerical success. Tiger's
renderer omitted `--native-tensor-input`, unlike the local native runner.
Connecting only a comparator would therefore have compared different inputs.

The User now calls `_prepare_yolo_input()` before publication: validate the
package/reference identity and hashes, load the registered fixture, perform
NumPy-only RGB/NCHW [0,1] bilinear preprocessing, and encode the native `images`
tensor. Custom input files must match these exact encoded bytes. The renderer
now supplies the native input flag, and the source archive includes the fixed
PPM and the new adapter reference module. No deployed Torch/model forward pass
was introduced; the legacy explicitly offline branch remains separate.

After receiving the actual response, `_record_yolo_numerical_result()` requires
exactly one native `predictions` tensor, canonicalizes the registered detection
rows and compares shape/classes/values to the precomputed oracle at atol=1e-3,
rtol=1e-4. Invalid values or mismatches block the User's successful marker.
Native decoding rejects duplicate/empty names and negative dimensions.
Remote error text is no longer printed on this path. Evidence is exclusively
created as `yolo-numerical.json`, with request/attempt/plan and content digests
plus aggregate errors; no plaintext tensors/detection rows are stored.
The exact component contract is `../contracts/yolo-numerical-evidence-v1.md`.

## Focused execution and evidence boundary

Initial red test: collection failed because `adapters.yolo.reference` did not
exist. After implementation, initial combined checks found one stale
source-location assertion (input validation moved into the production helper);
it was updated while executable helper and terminal-branch tests were added.

Final command:

```bash
python3 -m pytest -q --tb=short \
  tests/python/test_spec180_yolo_numerical.py \
  tests/python/test_spec180_yolo_application.py \
  tests/python/test_spec180_tiger_contract.py \
  tests/python/test_spec180_tiger_supervision.py \
  tests/python/test_spec180_release_workflow.py \
  tests/python/test_prepare_local_sif_source.py \
  tests/python/test_spec180_yolo_equivalence.py
```

**83 passed in 7.14s.** Relevant `git diff --check` also passes.
These tests cover synthetic response/reference fixtures, real production
input/evidence functions, and the unmodified production terminal branch with
transport/journal doubles. Success, numerical mismatch and remote failure
produce exit 0/4/3 respectively, without false success markers or error leaks.
Existing supervision tests use small subprocesses, not NFD/GPU workloads.
Source-archive tests prove file inclusion, not an image build or replay.

A read-only preflight of `.codex-tmp/spec180-yolo-candidate-current` passed:

- manifest SHA-256: `9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d`;
- oracle SHA-256: `ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175`;
- input shape [1,3,640,640], reference shape [1,50,6];
- NumPy versus the existing offline Torch fixture preprocessing: maximum
  absolute difference **1.1920928955078125e-7**, **not bit-identical**.

The fixture test executes preprocessing only, not a model. These results do
not establish real inference equivalence, GPU execution or protocol validity.
Manifest/reference hashes are byte bindings, not independent signature trust;
full candidate binding is still required. No package was regenerated/re-signed.

## Scoped convergence and remaining work

CodeGraph identifies the reference loader/comparator and User call edges;
exact source and executable terminal-branch tests confirm the comparison
controls the success decision. Generic graph queries also surface archived
`.codex-tmp` symbols; only canonical current paths were used as code authority.

The random-input mismatch, missing native argument and marker-only numerical
decision are repaired at source level. This closes only the numerical producer
portion of TP-02, not full T010/T013 or T014. Continue from:

1. **T007/T013:** emit actual native role/PID/ORT EP/physical GPU identity
   evidence. Startup arguments or intended backend names are not proof of
   successful CUDA execution; CPU Merge must remain GPU-free.
2. **T013:** implement the candidate-bound terminal collector consuming this
   numerical component, actual lifecycle/native execution evidence and observed
   supervision. Do not synthesize `spec180-result.json` just to satisfy the
   validator. Missing terminal evidence must continue to fail closed.
3. **T013 cleanup:** transient bootstrap/nfdc descendants/statuses, complete
   child closure, run-owned secrets/scratch disposal and redaction remain open.
4. **T011:** real critical Y-N negatives remain unimplemented; do not substitute
   focused tests for required real-network qualification.
5. Bind all changed candidate planes and rerun full T014; only PASS allows
   T015, followed by SIF/Tiger. Earlier source-only checkpoints do not bind the
   current changed subject. No new source archive/candidate seal was promoted.

## Resumption and tool gates

Context Mode project/active health passed at entry; repository Spec180 remains
checkpoint authority. CodeGraph, Spec Kit implementation/scoped audit and GSD
health validation were used. GSD reports healthy installation/state structure,
but its older phase-36/Spec170 route is not the active feature authority; this
report and tasks.md preserve the Spec180 continuation. ARS is not applicable
to this implementation-only repair. No full suite, model inference, real NFD,
container build/replay, upload, SSH, GPU allocation or scheduler job occurred.

## Changed source fingerprints (SHA-256, not a candidate seal)

| Current file | SHA-256 |
|---|---|
| `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/reference.py` | `8d4e1a15ce21369b1a1eb8c5d3d524ce8a65369327fe925703fbade8b039b320` |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` | `7d897c415af0caadcd0fa944eee1120b484087627d622f5aae9b0e263402777d` |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_2x2_lib.py` | `2e6e044191cf3b9732f0db9582d0fe0e74a231d98f2ebbd375757ca3d8d9ca4c` |
| `packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py` | `1fcc1c8fbf935cb4dcaca7066f018c163633bd5a0669465f0b042a8d63f54a40` |
| `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/prepare-local-sif-source.py` | `e95baa94bca47e68fc099700c9c9dc5002d9a422d9951059af0d3f3bc12c1084` |
| `tests/python/test_spec180_yolo_numerical.py` | `401eca24cedc6014b8423203750ccf43f2926623e7a078e861fe06d9f2a0a908` |

Existing modified/untracked work and frozen evidence were preserved. These
selected fingerprints are not a complete source/dependency/runtime seal.
