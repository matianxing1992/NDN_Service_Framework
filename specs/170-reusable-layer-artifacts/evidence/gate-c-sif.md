# Spec 170 Gate C Evidence (current checkpoint)

**Verdict: OCI PASS / SIF PENDING.** The immutable GPU OCI candidate has now
passed its full GitHub build, runtime probe, signature, SBOM, anonymous pull,
and release-manifest checks. This file still does not claim SIF or TigerCluster
execution.

## Immutable OCI checkpoint (2026-08-13)

The accepted OCI candidate and its evidence are recorded in
`gpu-release-20260813.md`:

```text
ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916
```

The exact SIF has not yet been materialized. The next authorized operation is a
bounded Slurm CPU conversion from this digest; no mutable tag or old Spec110/
Spec168 image may be substituted.

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

Gate C remains open until T024 produces one immutable OCI/SIF candidate and the
same candidate is executed in CPU/no-GPU mode with bounded CUDA preflights.
