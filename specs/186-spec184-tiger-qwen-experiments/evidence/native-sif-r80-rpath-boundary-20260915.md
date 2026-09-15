# Spec186 r80 SIF runtime-path boundary

- Candidate release: `spec186-4b91eee4-base1dd96267`.
- Source revision: `4b91eee4a7e7db0835c0a53ddfcc8befc6a21413`.
- Definition SHA: `sha256:4f2ca79904daf601bb987fc88edd42892194170f9610e61153248f3e356bc173`.
- Source seal: `sha256:28b3b33c1224c0c802d3f4c78b8e6ba35f5b219ab1bd2be7ed24f1660232dfcd`.
- Base SIF SHA: `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c`.
- SIF SHA: `sha256:34caa9cceba97a6b5c1098431ca0a99fdfacf44f17b17744a42fee2a47d5d7ad`.
- Apptainer: local `/usr/local/bin/apptainer` 1.5.3.

## Observed result

The container-native build completed `284/284` Waf targets with `./waf -j2`,
the builder Python/native imports and builder `ldd` checks passed, and the
SIF was created. A read-only probe of the exact SIF also passed imports,
provider `--help`, replay entrypoint presence, and missing-library checks.

The same probe found this extension RUNPATH:

```text
Library runpath: [/opt/ndnsf-di/current/lib:/src/ndn-svs/build]
```

`/src/ndn-svs/build` is a build-tree path and violates the portable SIF
runtime boundary even though the current loader could resolve the SONAME.
The candidate is therefore retained as `BUILD_PASS_RUNTIME_BOUNDARY_FAIL`;
it is not eligible for MiniNDN, upload, or Tiger execution.

## Correction

The Waf explicit NDN-SVS source/build pair remains in `LIBPATH` for link-time
selection, while runtime RUNPATH now uses the configured installed `LIBDIR`
only. A new source seal, rendered definition, SIF, and closure receipt are
required before promotion.
