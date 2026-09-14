# Spec186 development-SIF preflight — 2026-09-14

This receipt records the cheap cross-check added after the r8–r19 failed
candidate chain. It is a build-input gate only; it does not qualify MiniNDN or
TigerCluster execution.

| Field | Value |
| --- | --- |
| definition | `spec186-r6-runtime-v31.def` |
| definition SHA-256 | `2b4f3158c5ceef158f2921940e73a947d85c7df837a0c6e9b07c4c6c7a5eb672` |
| source seal | `sha256:f221cd8bac8318d35f7ee8da67765cf05e60b5dd79513b25ecb74b00e3bca833` |
| base SIF SHA-256 | `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c` |
| handoff files | 13, including the exact NumPy 1.26.4 wheel |
| Apptainer | `/usr/local/bin/apptainer` 1.5.3 |
| result | `SPEC186_PREFLIGHT_PASS` |

Command:

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-development-sif.py \
  --definition .codex-tmp/spec186-r6-runtime-v31.def \
  --apptainer /usr/local/bin/apptainer \
  --base-sif .codex-tmp/spec186-repaired-base-final.sif
```

The check confirmed that all rendered `%files` inputs exist, the sealed
workspace contains `NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in`,
the NumPy wheel contains exactly the three expected private DSOs, the definition
places them beside the base venv package required by its `$ORIGIN/../../numpy.libs`
RPATH, and NumPy 1.26.4 imports inside a writable overlay of the exact base SIF.
`build-local-sif.sh` invokes this validator before `LOCAL_SIF_BUILD_START`.

The r20 complete SIF build remains in progress; this receipt therefore does not
change T006 or any MiniNDN/Tiger runtime status.
