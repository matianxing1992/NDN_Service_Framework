# Spec180 T004 YOLO Export Evidence

**Status**: `PARTIAL_IMPLEMENTATION` (not a Provider or deployment qualification)

**Subject**: the registered `YOLO26n` checkpoint only. The exporter is
offline-only; PyTorch/Ultralytics are export-time dependencies and are not
runtime dependencies of the generated package.

## Checks

- Pinned source: `yolo26n.pt`, 5,544,453 bytes,
  SHA-256 `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`.
- Export contract: FP32, batch 1, static 640x640 input, ONNX opset 17,
  external initializers, provider-independent candidate catalogue.
- Dependency lock: `tools/ndnsf-di/spec180-yolo-export.lock`.
- Focused command:
  `python3 -m pytest -q tests/python/test_spec180_yolo_export.py`.
- Result: `6 passed`; one expected `torch.onnx` deprecation warning.
- Independent 640x640 exports from two clean temporary directories produced
  identical bytes: graph digest
  `sha256:956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a`;
  external-weight digest
  `sha256:1a998d3d56c0103e57ea6df557370a219a3df53380572b4e9337ff26b4a94a7f`;
  the previously recorded manifest digest
  `sha256:20bf6be3c07f668a2d068860ae03e6afe0e6caba54047bf53f0db53629ccb340`
  belongs to the pre-iteration-17 priority ordering and is invalidated.
  The repository-owned P3 fixture is identified in
  `tests/fixtures/spec180/yolo26n/README.md` by revision
  `spec180-fixed-fixture-v1` and digest
  `sha256:7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c`.
- The full-model oracle is stored only in each temporary export output (not
  committed): output shape `(1, 50, 6)`, digest
  `sha256:ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175`.
  Both independent exports produced the same oracle bytes. CPU ONNX Runtime
  reproduces the canonical rows within `atol=1e-3`, `rtol=1e-4` after the
  declared confidence filter and sort.
- Fail-closed checks: wrong checkpoint, mismatched registered signing key, and
  missing signing key for a configured signing operation fail before package
  acceptance or catalogue enumeration.

The current evidence proves deterministic canonical export and a repeatable
offline full-model oracle, including CPU ONNX Runtime equivalence after the
declared canonicalization. Iteration 17 changed the registered candidate
priority so the shared-backbone candidate is preferred when feasible; this is a
manifest-affecting change, so the previously recorded manifest digest is
invalidated and must be regenerated and signed with the registered authority.
The graph, external-weight, fixture, and oracle identities are unchanged. The
catalogue remains unsigned because the authority private key is intentionally
outside the repository. T004 therefore remains open until the regenerated
catalogue is signed and its evidence is recorded.
T005 must still implement and test the real adapter;
T006--T015 must prove ACK planning, Provider execution, and local qualification.
No Provider identity, GPU result, SIF result, or Tiger result is implied.
