# TigerCluster preflight: layered YOLO runtime

**Date:** 2026-09-09
**Scope:** substrate and container diagnostics only; no Spec183 YOLO qualification claim.

The target composition is the v22 base SIF plus the v32 external APP bundle:

- base SIF: `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`
- APP manifest: `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d`

The base SIF was not fully staged during this probe. The APP bundle was staged at
`/project/tma1/ndnsf-di/candidates/spec183-layered-v22-v32/app/`.

## Allocation and substrate

Allocation `210170` on `itiger03` recorded:

- GRES: one `rtx_6000` GPU
- GPU: NVIDIA RTX 6000 Ada Generation, 49,140 MiB
- driver: `560.28.03`
- Apptainer: `/usr/bin/apptainer`, `1.5.3-1.el9`
- scratch: `/tmp`, approximately 14 TB available
- 64 MiB `dd ... conv=fsync` write: PASS

The Apptainer semantic version matches the local `1.5.3` builder.

Allocation `210190` on `itiger02` and `itiger03` ran a two-task TCP probe:

- `itiger01`-style rank-0 listener accepted a connection from rank 1
- hostname resolution and the TCP payload round trip both passed
- no NDN, authorization, or YOLO conclusion is drawn from this raw transport probe

## Container diagnostics

Using the historical SIF
`/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`
only as a substrate diagnostic, with the current v32 APP mounted read-only:

- `_ndnsf` and `ndnsf_distributed_inference` import: PASS (`210174`)
- real YOLO `user.py --help` entrypoint: PASS (`210174`)
- APP Provider, fault Provider, and Controller `ldd`: no `not found` entries (`210178`)
- Provider `--help` returned exit 2 because that binary intentionally exposes
  `--check-only`/`--serve`, not a `--help` option; this is expected and is not a
  runtime success marker.

These results show that the compute node can run the APP boundary, but they do
not qualify the v22 base SIF or a distributed YOLO request.

## NFD socket failure and fix

Allocation `210203` started NFD with the stock `/etc/ndn/nfd.conf` under
`--containall` and a private home. NFD initialized its faces and TCP/UDP
channels, then exited with:

```text
filesystem error: UnixStreamChannel::listen: bind: Read-only file system [/run/nfd/nfd.sock]
```

This is the concrete cause behind the historical `NFD_READINESS_TIMEOUT` and
abort records in the older Tiger candidate. It is a container mount/configuration
failure, not an NDNSF-DI model failure.

Allocation `210205` used a generated per-run config with the Unix socket at the
writable bound path `/work/state/nfd.sock`. NFD stayed alive until the bounded
8-second timeout (`NFD_RC=124`), emitted no `FATAL`, `database is locked`, or
read-only error, and was recorded as `RESULT=PASS` for the socket-path fix.

The current Spec183 worker already generates this style of per-run config
(`runtime.baseline.nfd_config`/`runtime.yolo_worker`); the Tiger wrapper must
mount and pass that config instead of invoking the image's default config.

## Staging limitation

The 3.7 GiB v22 base SIF transfer to project storage sustained roughly 1.2--1.4
MiB/s and was canceled after about 255 MiB. Its partial file was removed; no
truncated SIF remains in the candidate directory. Repeated SIF uploads should be
avoided by content-addressed project caching or a one-time pre-stage. This is an
operational transport bottleneck and does not indicate a bad SIF.

## Verdict

The Tiger GPU, Apptainer version, scratch write, raw cross-node TCP, APP import,
and writable NFD socket path are now evidenced. The exact v22 base SIF has not
yet completed staging, and no single-node or two-node NDNSF-DI + YOLO request was
run. T012.b, T013.a, and T014.a therefore remain open.
