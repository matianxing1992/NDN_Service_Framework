# Spec186 r83 exact local SIF closure — 2026-09-15 (superseded)

> **Status: INVALIDATED.** The original build-time and first immutable probe
> passed, but a fresh remount later produced a SquashFS zlib/data-read error
> in NumPy (`SQUASHFS error ... zlib decompression failed`). This record is
> retained as failure history only; r83 must not be uploaded or executed as a
> candidate. A replacement build is required with the bounded mksquashfs
> setting and a second remount probe.

## Candidate identity

| Input | Value |
|---|---|
| source revision | `854c802379b63dc45a96b1a3caf2a7b4a872306f` |
| source seal digest | `sha256:348435f6ffa580cbac55bbb2d65a78528ff39732535b225489071c6512ddcbe2` |
| source-seal.json SHA-256 | `367b97c157f0b19edd4bec95fc16f5c9c05b479d1d986cbcfa8c9e570090433e` |
| base SIF | `.codex-tmp/spec186-repaired-base-final.sif` |
| base SIF SHA-256 | `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c` |
| base SIF bytes | `3,088,814,080` |
| rendered definition SHA-256 | `sha256:1c1fa780b11d3ce54e2a203f01c5122f6a266dc3440b6f9cd79e45174ebe14bd` |
| output SIF | `.codex-tmp/spec186-local-r83.sif` |
| output SIF SHA-256 | `sha256:275629970d771bee9f635099ec37d130fa89a8cff9211444d81d78de5ec7a7c2` |
| output SIF bytes | `3,406,168,064` |
| Apptainer | `/usr/local/bin/apptainer` 1.5.3 |

The build record reports `status=PASS`, `containerNativeBuild=true`, the
matching base/definition identities above, and no host binary inputs. The
source archive contains 522 sealed files.

## Pre-build and build gates

- `preflight-development-sif.py` passed against the r83 definition and base:
  `SPEC186_PREFLIGHT_PASS wheels=.../spec186-source-handoff-r83/wheels`
  with 11 workspace consumers.
- Native Waf target closure passed `284/284`.
- The two Python wheels were built in the container and consumed by the final
  stage; builder imports and native `ldd` checks passed.
- The final-stage verifier, replay/source-seal check, and packaged `nfd`,
  `nfdc`, and `ndnsec` checks passed before SIF creation.

## Immutable SIF runtime probe

The probe used Apptainer 1.5.3 with `--cleanenv --containall --no-home
--pwd /tmp`, and set only temporary `HOME`/XDG directories. It deliberately
left NDN keychain variables unset so host-specific TPM selectors could not
change the result.

- Ten Python imports passed: `_ndnsf`, NDNSF-DI, Repo, NDN, tokenizer, ONNX,
  NumPy, ONNX Runtime, and both native helper modules.
- `ldd` found no `not found` entries for both native providers, `_ndnsf.so`,
  and all nine packaged shared libraries.
- `readelf -d` found no `/src/`, `/tmp/`, or `/home/` RPATH/RUNPATH leak in
  those ELF objects.
- `di-native-provider --help` returned success.
- `NDNSF_DI_YoloAckDriven_Minindn.py --help` returned success.
- `/opt/ndnsf-di/replay/source-seal.json` was present.

This is a local immutable build/loader closure result. It does not execute a
MiniNDN YOLO campaign, prove a GPU backend, qualify the Qwen3-0.6B model, or
prove immutable upload, Tiger single-node, two-node, or independent reuse.
Those Spec186 gates remain open until their own candidate-bound evidence exists.
