# Spec186 Tiger r38 build and first-launch boundary — 2026-09-14

This receipt records the first compute-side r38 attempt after the local SIF
materialization path was stopped for a verified zstd-helper incompatibility. It
is a build and first-failure receipt, not a MiniNDN or GPU qualification result.

| Field | Value |
| --- | --- |
| Slurm job | `212306` |
| compute node | `itiger02` |
| GPU | NVIDIA RTX 6000 Ada, UUID `GPU-e7e0d43c-6eb4-9df7-c3fb-761636537829`, 49140 MiB |
| compute Apptainer | `/home/tma1/.local/bin/apptainer-1.5.3`, version 1.5.3, SHA `a73ab497f71e4371ddad0bc4211c46b78bdfb491b10e9640e16d5cfaa090a8c4` |
| login Apptainer | `/usr/bin/apptainer` 1.3.4, metadata-only |
| source seal file | `/home/tma1/.cache/spec186-r35/source-handoff/source/source-seal.json`, SHA `053a53b3963f08b9abd3cfedb4ef7ecf0f1d8b02be51f700a381e8466e23596e` |
| app bundle | `spec186-app-bundle-r6`, digest `04c2dd64b4f070cbd909a87f75a0372a0e3d4dae45c7e712641369cf76531d73` |
| intermediate SIF | `/project/tma1/ndnsf-di/candidates/spec186-r38-runtime/intermediate-base.sif`, SHA `328f2ffd2b0d8da1c19e4114c196bea255f7ee1f626f25f891398a046e012a27` |
| final SIF | `/project/tma1/ndnsf-di/candidates/spec186-r38-runtime/final.sif`, 4,167,950,336 bytes, SHA `e890b5de2ffffb9e795cd9b2a7cce28b58cef4b7adcb9fe13844bcfa47c168dd` |
| case input | `candidates/spec180-runtime-b6710fd6/models` (historical staged input; not a Spec186 qualification receipt) |
| model graph | `canonical-package/canonical/yolo26n.onnx`, SHA `956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a` |

## Verified

The compute job completed the fresh 284-target native build, produced the final
SIF with Apptainer 1.5.3, and passed the final runtime native import check:

```text
LOCAL_SIF_BUILD_PASS sif=/project/tma1/ndnsf-di/candidates/spec186-r38-runtime/final.sif sha256:e890b5de2ffffb9e795cd9b2a7cce28b58cef4b7adcb9fe13844bcfa47c168dd apptainer=apptainer version 1.5.3
SPEC186_R38_FINAL_BUILD_PASS bytes=4167950336 sha256:e890b5de2ffffb9e795cd9b2a7cce28b58cef4b7adcb9fe13844bcfa47c168dd
SPEC186_R38_FINAL_RUNTIME_IMPORT_PASS
```

The final command was:

```bash
apptainer exec --nv --cleanenv --containall \
  --bind "$STATE:$STATE:rw" \
  /project/tma1/ndnsf-di/candidates/spec186-r38-runtime/final.sif \
  /opt/venv/bin/python \
  /opt/ndnsf-di/replay/repo/Experiments/NDNSF_DI_YoloAckDriven_Minindn.py \
  --case Y-A
```

## First failure and scope

The runner stopped before MiniNDN startup with:

```text
SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT error=CANONICAL_CATALOGUE_VERIFY_FAILED
```

The allocation ended with Slurm exit 78 after 28m45s. No ACK/Selection,
dependency Data, terminal response, numerical oracle, observed CUDA model-role,
or cleanup receipt was produced. Therefore this job cannot close V05, V06/V07,
V09, or any two-node row.

The same final image passed a direct `cryptography` import diagnostic and a direct
`ModelFamilyAdapter` catalogue diagnostic. Those lower-scope checks show that the
SIF build and basic catalogue dependency are not by themselves the qualification
failure; the runner's original exception is still hidden by its wrapper and must
be captured before the next retry. Preserve this SIF and receipt; do not blindly
rebuild or classify the result as a GPU run.

## Promotion correction

r38 proves an explicitly recorded compute-build fallback, not the default
promotion contract. The next candidate must either complete local Apptainer 1.5.3
build/import/CPU smoke, upload the exact sealed bytes and verify the same SHA on
compute, or record a new local materialization failure and a new compute-built
source-sealed candidate before any MiniNDN/Tiger qualification attempt.
