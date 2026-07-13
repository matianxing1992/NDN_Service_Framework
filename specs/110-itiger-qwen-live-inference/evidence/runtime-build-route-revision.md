# Runtime build route revision

**Recorded**: 2026-07-13
**Evidence level**: `MEASURED` for GitHub Actions failures and iTiger login-node
discovery; `PROPOSED` for the compute-node rootless build until T046 executes.

## Preserved GitHub Actions result

The optional workflow `.github/workflows/ndnsf-di-itiger-image.yml` was repaired
through checkout and Python/bootstrap failures, then reached its dependency clone
step. Run `29243531859` failed with exit 128 because these pinned commits are not
fetchable from their configured public remotes:

- NFD `2b43d675e3fba37b5362fab0001ba542bb07b43b`
- ndn-svs `5b5461a728012e9d0959e99ef0acbc5f32fc9d25`

The earlier diagnostic run `29243293121` and its predecessors are retained in
GitHub Actions history. None submitted an iTiger Slurm job, entered an NDNSF-DI
GPU stage, or count as an acceptance attempt. The failure is not rewritten as a
runtime or inference result.

## Revised primary route

1. Seal the repository and pinned dependency sources under project storage.
2. Render and review a bounded Slurm CPU build job.
3. Select job-unique compute scratch; set explicit rootless container graphroot,
   runroot, cache, and temporary paths there.
4. Build/export a digest-bound OCI archive with rootless Podman/Buildah.
5. Verify and atomically promote the OCI archive to project storage.
6. Materialize and checksum the SIF with Apptainer.
7. Run the separate exactly-once GPU runtime probe before any Qwen acceptance
   cell.

Live login-node discovery found Podman 5.2.2 (rootless), Buildah 1.33.7,
Apptainer 1.3.4, Slurm `TmpFS=/scratch`, and approximately 14.5 TB advertised
node `TmpDisk`. Podman's default graphroot is under `/home` and is forbidden for
the build. These observations do not prove compute-node capability; T046 is the
single admissible diagnostic for that boundary.

## Rollback and stop conditions

- GitHub Actions remains an optional publication mirror and may be re-enabled
  only after all pinned sources are remotely fetchable or supplied as a sealed
  build input.
- A pre-start Slurm or tool-availability blocker leaves T046/T047 open.
- A post-start diagnostic failure is preserved once; no automatic resubmission
  under the same identity.
- No full runtime build starts until the tiny rootless OCI-to-SIF diagnostic
  passes and its durable evidence checksum verifies.

## T044 implementation evidence

Implemented on the existing Slurm/Apptainer and OCI owners:

- checksum-bound `release build-render` with no submission side effect;
- deterministic compute-scratch selection and explicit Podman graphroot,
  runroot, `XDG_CACHE_HOME`, `TMPDIR`, `HOME`, and runtime directory;
- exact Git archive source seals for the workspace and all locked dependencies;
- sealed dependency mode in the existing GPU Dockerfile rather than a second
  build graph;
- OCI archive manifest validation, SIF conversion/execution, atomic durable
  promotion, partial cleanup, signal handling, and fail-closed evidence writes.

Verification on 2026-07-13:

- Spec 110 Python offline suite: 73 tests, 0 failures/errors/skips;
- rootless builder integration: `ROOTLESS_BUILD_PIPELINE_PASS`;
- existing release pipeline: `RELEASE_PIPELINE_PASS`;
- runtime compatibility: 6 cases PASS;
- packaged network and security contracts: PASS;
- frozen T001–T030 foundation validator: PASS (its original 49-test JUnit is
  intentionally not overwritten by the later 73-test suite).

This closes implementation task T044 only. It does not upgrade the login-node
tool observations to compute-node evidence and does not close T045 or T046.
