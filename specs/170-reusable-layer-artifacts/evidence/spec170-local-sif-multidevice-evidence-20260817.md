# Spec170 local SIF: multi-device execution evidence — 2026-08-17

This candidate was built locally with Apptainer 1.3.4 from the sealed source;
TigerCluster is expected to verify the hash and execute the SIF only.

| Item | Value |
|---|---|
| Release | `spec170-runtime-52ad67fd-multidevice-20260817-r1` |
| Source revision | `52ad67fdfcd10b21e5c467bada86b3dca0e96428` |
| Source seal | `sha256:1762f19ad6b229c62b70504af8e2b10c5360b9af28937f8e03456afb6f34b998` |
| SIF | `/tmp/spec170-release-52ad67fd-multidevice-20260817/runtime.sif` |
| SIF SHA-256 | `sha256:3fa4bc6d411e8b19ebaabd63879e584b405c7ab201a660f19b19d64d3353b68e` |
| Provider SHA-256 | `sha256:63f223ec2ccd8621bdbfe465a453dc2fe03df884de1a267a877bc2c37f05b283` |
| SIF bytes | `4,398,845,952` |

The code change keeps scalar single-device evidence and adds optional
`device.ids`/`gpuUuids` arrays. Aggregate evidence now represents a Provider
whose roles use different CUDA devices as `device.id="multi"` and retains the
individual IDs for audit; mixed-device roles no longer fail the internal
consistency check solely because their device IDs differ.

Local checks on this exact SIF:

- static runtime probe: PASS;
- Python imports: Torch `2.6.0+cu124`, ONNX Runtime `1.20.0`, CUDA providers
  visible in the runtime (host has no local GPU, so `torch.cuda.is_available()`
  is `False`);
- provider binary is present and its hash matches the build input: PASS;
- `ldd` closure: all resolved libraries are inside the expected `/opt` or
  pinned system runtime paths; no `not found` entries: PASS.

The Provider `--check-only` invocation was not used as acceptance evidence for
this record because the temporary manifest pair was not the formal network
bundle. D2a/D2b/D2h still require the real hash-bound bundle and Tiger
request/ACK/Selection/Response evidence.
