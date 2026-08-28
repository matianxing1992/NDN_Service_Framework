# Spec 111 to Spec 110 iTiger Handoff

**Status**: DESIGN HANDOFF — no post-Spec-111 OCI/SIF or Slurm job exists

## Decision

The deployment is logically feasible on iTiger only through:

```text
sealed Docker/OCI build source
  -> exact SIF materialization
  -> Slurm allocation
  -> apptainer exec --nv
  -> containerized NFD/controller/generic Providers
  -> Spec 111 APPDeployment apply/READY/ACTIVE
  -> APPClient submit/result
  -> drain/INACTIVE/teardown
```

Docker is not the iTiger compute runtime, and an iTiger allocation is not an
always-on public-IP service.

## Current verified boundary

- The current Spec 110 source/image predates the implemented Spec 111 workflow;
  it cannot be relabeled as post-separation evidence.
- T213 published an immutable OCI release, but T214/job `149669` failed before
  SIF download because the compute node could not resolve UID `64102`; no SIF
  was promoted.
- T217 records that a separate one-minute preflight job `150145` passed UID and
  Apptainer checks on `itiger07`, but no replacement materialization identity is
  frozen and T218 remains open.
- T215 GPU runtime validation and T216 substrate verdict remain open. Therefore
  no current iTiger GPU, ONNX CUDA or Qwen inference PASS exists for this path.
- `run-container.sh` validates exact SIF and least-privilege model/artifact/
  identity/scratch binds, but has no Spec 111 persistent `/state` or shared
  node-run bind.
- `SlurmApptainerAdapter.render_sbatch()` still renders one workload command;
  Spec 110 T063 process-map wiring is open.
- allocation topology v1 fixes three Providers and its generated project
  commands are not intrinsically wrapped by the exact-SIF runner. Those
  properties are valid only as frozen pilot fixtures, not as the generic Spec
  111 deployment bridge.
- the single-node Qwen launcher starts NFD/controller/providers/user in one SIF,
  but it does not call the proposed Spec 111 revision/apply/durable-handle
  workflow and cannot close the new acceptance boundary.

## Canonical contract

The normative bridge is:

`specs/111-ndnsf-di-core-app-separation/contracts/itiger-slurm-apptainer-handoff.md`

It requires:

- a new post-Spec-111 OCI/SIF identity;
- immutable `RuntimeAllocationHandoff` and distinct infrastructure handle;
- revision-derived Provider roles rather than generic fixed-three logic;
- every project process inside the same verified SIF;
- exact read-only model/artifact/role-identity binds;
- identity-partitioned persistent RuntimeJournal/request-spool bind;
- one shared node-local NFD run bind and per-Provider GPU UUID mapping;
- separate Slurm, APPDeployment and request state;
- single-node small-Qwen PASS before selected-transport multi-node work.

## Execution ownership

- Spec 111 T001-T201 implements and validates Core/APP separation and emits the
  final candidate/revision/offline-gate identities using local package/static
  checks and MiniNDN distributed acceptance. It builds no OCI/SIF, starts no
  container runtime and authorizes no Slurm/iTiger job.
- Spec 110 T219-T228 implements and proves the offline Slurm/Apptainer bridge.
- Spec 110 T229 creates the new OCI/SIF candidate only after Spec 111 completion,
  substrate PASS and explicit external-publication/materialization authority.
- Spec 110 T230 is the first eligible live post-Spec-111 single-node use.
- Spec 110 T231 is optional until T064 selected-transport and T230 pass.
- Spec 110 T232 records the resulting boundary without completing unrelated open
  Qwen-size/performance work.

## Finalization required by Spec 111 T200

Replace this design-only status with exact completed Spec 111 source,
candidate, deployment-revision, offline-gate and package/image-input digests.
Re-audit T219-T232 against the implemented API before any render. Do not reuse
an old authorization, job identity, OCI/SIF digest or result bundle.
