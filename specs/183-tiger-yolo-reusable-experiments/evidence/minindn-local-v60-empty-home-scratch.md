# Exact-SIF empty HOME/scratch isolation v60

**Date:** 2026-09-09

**Run:** `minindn-local-20260909-v60-empty-home-c`

**Verdict:** `PASS`, `qualification=LOCAL_SIF_ISOLATION_COMPONENT`

This is the independent V13 isolation probe paired with the v58 normal
MiniNDN YOLO collector run and the v59 Y-N aggregate. It uses the same base
SIF and external APP, starts with an empty host HOME directory, and exercises
the writable scratch path inside the exact Apptainer composition. It is an
isolation/runtime-boundary result, not a GPU or TigerCluster qualification.

## Frozen composition

- base SIF: `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`
- base SIF bytes copied and verified in scratch: `3901079552`
- external APP manifest: `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`
- Apptainer: `1.5.3`
- raw evidence: `Experiments/TigerCluster/results/minindn-local-20260909-v60-empty-home-c/`

The probe used `--cleanenv --containall --home /node/home`, bound the run
directory read/write and the APP read-only at `/app`, and bound the source SIF
read-only as `/node/source.sif`. NFD used the writable scratch socket
`/node/scratch/nfd.sock` generated from the maintained baseline configuration.

## Observations

The host HOME was empty before entering the container (`before_home=0`). The
probe then completed all of the following inside the SIF:

- wrote and fsynced a scratch probe;
- copied all `3901079552` SIF bytes into scratch and matched the pinned SHA-256;
- imported `ndnsf` from the base runtime;
- ran the v32 User `--help` entrypoint with exit `0`;
- started NFD and observed the scratch Unix socket;
- stopped NFD with exit `0`, with no read-only-filesystem, database-lock, abort,
  fatal, or error markers;
- removed the scratch copy and left the scratch directory empty.

The import created the expected NDN PIB under the otherwise empty HOME
(`homeEntriesAfterImport=1`); no host HOME content was available to the
container. The checked-in compact result is `probe-result.json`; the complete
stdout/stderr is `logs/apptainer.log`.

## Scope and next gate

Together with [v58 exact-SIF local PASS](minindn-local-v58-exact-sif-pass.md),
this closes the V13 local CPU/empty-HOME/scratch evidence gap for the unchanged
base+APP composition. It does not change the host receipt's
`YOLO_HOST_GATE_COMPONENT_ONLY` label or close T007's formal ordering. The
next gate is one content-addressed staging of this exact SIF and APP to Tiger,
followed by a bounded single-node GPU preflight and request.
