# Spec 170 Gate C Evidence (historical checkpoint)

> **Superseded.** This ledger records earlier Tiger-side build attempts and
> must not be used to select a new runtime. The current Gate-C release pointer
> is the locally built and verified SIF recorded in
> `local-sif-build-route-20260817.md` and `evidence/README.md`:
> `spec170-runtime-dd5c11cc-localfix-20260817-r1`, SHA-256
> `f6521a8226190279cb961f2e100245d783c8ba90723a4b004f1f773df71b5874`.
> Tiger now verifies and executes that locally built SIF only; it does not
> build it. The older SIF below is historical negative evidence.

**Verdict: CONDITIONAL — exact SIF verified; clean wrapper materialization still
open.** The original no-SIF record below is retained as a historical
preflight. The current r5 SIF is real and hash-verified, but its rootless build
wrapper exited nonzero while cleaning Buildah scratch, so this is not a clean
rootless-build PASS.

## Local exact-SIF availability check (2026-08-05)

The repository was checked before any exact-SIF claim:

```text
find results -type f \( -name '*.sif' -o -name '*.img' \)
  -> no Spec170 SIF artifact
command -v apptainer
  -> unavailable on this host
```

The existing local Docker image is a Spec168 image and is not substituted for
the T024 candidate. No SIF was rebuilt or retagged, and no TigerCluster job was
submitted.

## T024 implementation checkpoint

The deployment-side CPU/GPU boundary now has the following source contracts:

- `run-container.sh --gpu-count 0` omits Apptainer `--nv`; positive counts add
  `--nv` and always verify the supplied SIF SHA-256 before execution.
- Slurm rendering omits `#SBATCH --gres` for zero-GPU profiles and passes the
  explicit GPU count to the container launcher.
- `spec170_allocation_topology.py` rejects gate/profile mismatches, hidden
  defaults, invalid digests, and cross-Provider-to-local-GPU relabeling.
- Immutable D0/D1/D2a/D2b/D2h job entrypoints and exact-SIF mutation tests are
  present, but they have not been run against a materialized T024 SIF.

The source-only checks passed:

```text
bash -n specs/170-reusable-layer-artifacts/jobs/gate-d*.sbatch \
       packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh
pytest -q tests/container/unit/test_spec170_allocation_topology.py \
         tests/container/unit/test_spec170_exact_sif_gate.py \
         tests/container/unit/test_slurm_render.py \
         tests/container/unit/test_slurm_submit.py \
         tests/container/unit/test_slurm_node_scripts.py
13 passed
```

The source-only checkpoint hashes are:

```text
c08b76fc5a31e90817d569dc5c77c82d967cd7b72d2b9a85796b35489a2e0629  packaging/ndnsf-di-container/lib/spec170_allocation_topology.py
144625f559bf6fc7246c65121cb30ac52307950bc220378c84b6f74fc3573499  packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh
b27a5de77cb4fee6ac12f43401829a04a2ee055c6ab4b7734f4be3861212b1a1  packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py
0decd888b55c81c49fdb860f6873b1d3cc1cdb497714db4840ab535c03854fcd  packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/ndnsf-di.sbatch.in
4440b2a62b481676d1f4f18b4469ca632e741df96d2a174ac9ae5abc9ff72ab8  specs/170-reusable-layer-artifacts/jobs/gate-d0-cpu.sbatch
528ec67fb0bd89447af082e3ae2959dc277acb4431d31b92a470eadc197058e8  specs/170-reusable-layer-artifacts/jobs/gate-d1-single.sbatch
769cc40786ca5a8208c1fc4f51964577da43c73169ecb65b18301bcdbf8e9985  specs/170-reusable-layer-artifacts/jobs/gate-d2a-local-two-gpu.sbatch
c7263f2bdce939689ce654fc7e54857698cdc8139137c6c751dd7b9ec0a3bc88  specs/170-reusable-layer-artifacts/jobs/gate-d2b-cross-provider.sbatch
23493a9f6a0282266d49b7f68364de978bd2770ca167aa82906f3674fccaca30  specs/170-reusable-layer-artifacts/jobs/gate-d2h-hybrid.sbatch
96cb59f32b1c2a296246bc17bd0327f8a228be5555d032fb9d05aed244e2dfc6  tests/container/unit/test_spec170_allocation_topology.py
7b18323a045b17fe063c39199ce81807c339118ab50d9da8e379be0dd74d7bb2  tests/container/unit/test_spec170_exact_sif_gate.py
```

## Current r5 materialization and exact-SIF verification (2026-08-16)

The sealed source identity is `0142d4ee2310de056aa391d954f2c3db8fb62023` with
source seal `sha256:0b2ea4a56713d4a2b4e36bdc47edba568cfcb7c4dab7e1db3b2bf198d5ac5462`.
Tiger job `196525` produced the final OCI and SIF and passed the static probe,
but returned `ROOTLESS_BUILD_SCRATCH_CLEANUP_FAILED` while removing its
rootless Buildah graphroot.  The release is therefore classified as
`artifact-ready / orchestration-failed`, not as a clean build-wrapper PASS:

```text
release: /project/tma1/ndnsf-di/releases/spec170-runtime-0142d4-gpu-20260816-r5
runtime.sif bytes: 4397842432
runtime.sif sha256: 490ff5fbf20ef3be56caf398478f457efc2a786fdeaf41e90d1ccbbc9addafb6
runtime.oci.tar sha256: bf2c73d5086bbec15d4a6c2e85e0e6d7bed136c2d75dfa42aa93cf378dd96422
static probe: PASS
build wrapper: FAIL (scratch cleanup only; payload was produced)
```

Independent Tiger job `196669` then verified the exact SIF by path and hash
without rebuilding it.  It passed the runtime probe, Python imports, ORT 1.20.0
and Torch 2.6.0+cu124 checks, and recorded
`SPEC170_SIF_VERIFY_PASS jobId=196669`.  This closes exact-SIF availability and
static runtime integrity, but it does not claim the D0/D1/D2a/D2b/D2h network
features; those still require current-SIF request/ACK/Selection/Response
workloads.

The current r5 SIF was rehashed after the targeted Tiger storage cleanup and
still matches the value above.  Superseded SIFs, old Spec162 duplicate model
stages, and rebuildable Apptainer cache were removed separately; current r5
release metadata, hashes, source seals, and failure evidence remain retained.

Gate C is therefore open for the next bounded current-SIF CPU/no-GPU network
smoke, while the scratch-cleanup wrapper defect remains a packaging follow-up.
