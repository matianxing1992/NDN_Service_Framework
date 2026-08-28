# Spec170 local candidate r13 qualification (REVOKED)

Date: 2026-08-17

> **REVOKED on 2026-08-18.** This candidate copied a host-built
> `ndnsf/_ndnsf.so` into the SIF. The checks below established final-image
> import/link/runtime behavior, but did not establish container-native build
> provenance. Under the current Spec170 build-boundary contract, a host-built
> container extension is `WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT` and cannot
> be qualified by a later import, `ldd`, hash, or functional PASS. Do not
> upload, execute, or use r13 as a base for a new candidate.

Release: `spec170-runtime-31d9f547-post-selection-v11-20260817`

## Identity

- Source revision: `31d9f547a32f941ab948f005f583c429d23bacbb`
- SIF: `/tmp/spec170-local-candidate-31d9f547-20260817-r13/runtime.sif`
- SIF bytes: `4398850048`
- SIF SHA-256: `4e29b0fdc14eba8172fb927dcea07676e807f1b577ea76a82b786b407cbc2840`
- Source seal SHA-256: `aee2ea8106b01208754a43f340d1d0ee4a25102bcd48c362a0a5bff546def53e`
- Library lock SHA-256: `3fc33dbadcd8d3a638dd929b10032119ba7c14622a9301df1905aa546f3eec12`
- Local build record SHA-256: `63f65e3596a78ae50b502029aadb4bf55c3c9df98e1dacbfdc32804e797c9f04`
- Build-record body digest: `sha256:e47de4576feacfe2ab73df29ff47c84dbf3a676fbc79700ee510fe0238dd35f3`
- Base SIF SHA-256: `bd949732fc89bb10ef48e92b51fb4557fb33819797882765bd629c73da43748c`

## Apptainer contract

- Compute-node expected package version: `1.5.3-1.el9`
- Local semantic version: `1.5.3`
- Local executable: `/opt/apptainer/1.5.3/bin/apptainer`
- Local executable SHA-256: `f491c4f4f8d17dccd4a540a5a40d84880e95a3d122b784aaf754c2faaf469f9b`
- Tiger action: `verify-hash-and-execute-only`

The expected version is derived from the bounded compute-node probe on
`itiger08`; the login-node `1.3.4` installation is not release authority. The
previous r12 SIF is locally qualified but rejected for Tiger because it was
built with login-node-matched Apptainer 1.3.4. It was not used for D0, D1, or
D2 execution.

## Exact-SIF closure

- Declared v11 labels: PASS.
- One active Python 3.10 `_ndnsf.so`: PASS.
- Python native import: PASS.
- ONNX Runtime `1.20.0`: PASS.
- Available ORT providers: TensorRT, CUDA, CPU.
- Provider SHA-256: `156ae2457c3259cf56e0752b2d84621dd2eba552b5d119101e1482c7f913f0cc`.
- Framework SHA-256: `2efdadd8534b54053eb52a423aaf300276587f3ecb0436faf5eb29faa46039b9`.
- Python extension SHA-256: `0e1b62491cdac55cc94aaeb9f141f0f4e7fefbcc48abcf503838aeb5c3a09a37`.
- Provider `--check-only --wiring-check-only`: PASS, four roles and four
  artifacts loaded.
- Complete library closure: PASS, 18 locked libraries, two primary targets,
  no failure. Evidence SHA-256:
  `7a8f9444365a90c122826aa41db4b2ec2b9902980dd86913e6ea2b78d451db42`.

## Exact-SIF functional gate

CPU bundle manifest SHA-256: `1727328ca7f7aa458833a3c88f45865b090d8052d1455f1c893845949fed9ac4`.
Every file listed by `bundle.sha256` verified before execution.

The exact r13 SIF passed the focused candidate gate:

```text
3 passed, 6 deselected in 25.56s
```

The three cases verify the Provider CLI contract, the D0 four-Provider chain,
and the D1 single-Provider chain. Both network cases reached Selection,
executed all four planned dependency edges with the real ORT CPU runner, and
delivered a non-empty final Response.

## Decision

Historical local result: **PASS under the incomplete pre-provenance gate**.
Current release decision: **REVOKED / NOT QUALIFIED**. The candidate must not
be copied to Tiger project storage, executed, or used as a base image. A new
candidate must be built through `build-local-sif.sh`; its `_ndnsf.so`, Provider,
and framework library must be produced by the sealed builder stage and matched
to `container-native-build.json` inside the exact final SIF.
