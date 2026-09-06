# Spec180 revision 109: native-library closure diagnosis

Date: 2026-09-03  
Status: `BLOCKED_AT_G0` / pre-start readiness failure

## Observed attempt

The first developer Y-A launch using the barriered MiniNDN runner started the
Controller and Repository phases, then both native Python clients terminated
with `socket read error (End of file)`. No request, ACK, Selection, Provider
execution, or terminal Response was produced.

## Linkage evidence

The Python extension and NAC-ABE resolved:

```text
/home/tianxing/NDN/ndn-service-framework/.local-boost171/lib/libndn-cxx.so.0.9.0
sha256:508f0fd9020947402f9b5c5b654ebe85541c097293ea3f625bfdbcbb7bc3a712
Build ID: 3663a168d0d5c51c5046c3b91dd372688b2bdd99
```

MiniNDN NFD and the current NDN-SVS library resolved:

```text
/usr/local/lib/libndn-cxx.so.0.9.0
sha256:cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531
Build ID: 189995208664f28f4001cbe08c668d5e2b48937e
```

The same soname therefore did not identify one ABI/toolchain. Forcing
`LD_LIBRARY_PATH` is not an accepted repair: it can load stale installed
framework/SVS libraries or leave the extension and NFD on different builds.

## Gate correction

`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` now runs
`_validate_native_library_closure()` before MiniNDN/NFD creation. It compares
resolved paths and file digests for the extension, NFD, NDN-SVS, and NAC-ABE;
the entrypoint reports `WAITING_EXTERNAL_INPUT` with exit 78 on mismatch.
The focused rejection test passes, and a direct rerun now stops before network
creation with the explicit mismatch rather than producing another socket EOF.

## Required recovery

Build NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python extension from one explicit
ABI-identical ndn-cxx/Boost/toolchain closure. Record every resolved path,
digest, and Build ID in the candidate manifest. Then rerun exactly one Y-A
request. This evidence is a readiness diagnosis, not a protocol result, and
does not authorize Y-B/Y-N, SIF, or TigerCluster.
