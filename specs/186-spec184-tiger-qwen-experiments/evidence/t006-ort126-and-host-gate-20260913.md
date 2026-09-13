# Spec186 T006 recovery probe — 2026-09-13

## Scope

This receipt records a bounded recovery probe after the old Tiger SIF was
rejected. It does not promote a SIF or close T006: the temporary base is made
from the historical v23 image and the current host M01 qualification did not
reach MiniNDN startup.

## Temporary Apptainer 1.5.3 base probe

The local experiment host used only `/usr/local/bin/apptainer`, which reports
`1.5.3`. A temporary localimage build injected the available ORT 1.26 SDK into
the historical v23 base:

| Input | Value |
| --- | --- |
| recipe | `/tmp/spec186-base-ort126.def` |
| output | `/tmp/spec186-base-ort126.sif` |
| SIF bytes | `3652956160` |
| SIF SHA-256 | `sha256:a427e583261738ef3224d5881d808f0bb9f202c1cacc46955736e25161e83482` |
| runtime | Apptainer 1.5.3 |
| app bundle | `badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d` |

With the app bundle mounted read-only, `bin/di-native-provider --help`
returned zero and printed the native usage contract. `ldd -r` in the same
composition reported no `not found`, `undefined symbol`, or loader error. The
temporary base still contains historical Core/SVS/NAC/NDNSD components, so
this is loader evidence only and is not a source-sealed runtime qualification.

## Host M01 attempt

The real host/CPU wrapper was invoked once with `M01`, seed `1750001`, four
Providers, and the checked-in tiny ONNX fixture. It failed before MiniNDN
startup while importing the staged Python extension: the loader selected the
older `/usr/local/lib/libndn-service-framework.so.0.1.0`, while the staged
`libndnsf-distributed-inference.so` requires newer framework symbols including
`ServiceRegistration::closed`. The run directory is
`Experiments/TigerCluster/results/spec186-host-gate-20260913/M01`; it is a
failed diagnostic, not a host-gate manifest.

Rebuilding the host framework with the pinned NAC-ABE and NDN-SVS prefixes
then stopped at configuration because the previously used full-protobuf ONNX
development prefix `/tmp/spec186-onnx-prefix2` is absent and the ORT 1.26
runtime SDK does not provide `onnx/checker.h`. No host-gate manifest was
fabricated and no old manifest was reused.

## Result and recovery boundary

The temporary probe advances ABI diagnosis but leaves T006.c and all runtime
qualification rows blocked. The next executable step is to provide the pinned
full-protobuf ONNX development prefix, rebuild the current Core/DI/extension
closure, rerun one real host M01, then invoke the canonical
`build-local-sif.sh` with a current host-gate manifest and
`--expected-apptainer 1.5.3`.
