# Current sealed SIF verification (2026-08-19)

This record separates “the image runs” from “the image contains the current
working tree.”  The candidate below is usable only for the sealed source
revision recorded in its build record.  It is not a claim that the dirty
working tree has already been packaged, and it is not a T029/T036 completion
record.

## Candidate identity

```text
candidate:       spec170-local-candidate-r23-permission-replay-marker
sourceRevision:  989a9daace669a4f93496dade3176c527edb2469
sourceSeal:      sha256:348a797c746bf91d097f04627e0da524511cdcf885c9c22dbc94a0c770c7059e
definition:      .codex-tmp/spec170-container-build-20260818-r14/spec170-local-candidate-r23.def
buildRecord:     .codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
build entry:     packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh
SIF:             .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SIF bytes:       4442767360
SIF SHA-256:     5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
Apptainer:       /opt/apptainer/1.5.3/bin/apptainer 1.5.3; target record 1.5.3
build boundary:  container-runtime-in-sif; containerNativeBuild=true
host inputs:     []
tiger action:    verify-hash-and-execute-only
runtime Python:  /opt/venv/bin/python 3.10.18
SOABI:           cpython-310-x86_64-linux-gnu
EXT_SUFFIX:      .cpython-310-x86_64-linux-gnu.so
```

The workspace currently has uncommitted/generated entries.  Therefore this
SIF must not be used for a newer checkout or for any uncommitted runtime
source change.  A source, dependency, definition, native-library, or binding
change creates a new candidate identity and a new complete local SIF; the old
image remains a control only.

## Exact-image probes

All probes were run against the exact SIF above with the explicit
`/opt/apptainer/1.5.3/bin/apptainer` (1.5.3):

```text
/opt/venv/bin/python --version                         PASS (3.10.18)
import ndnsf; import ndnsf._ndnsf                       PASS
active _ndnsf*.so census                               PASS (exactly one)
_ndnsf.so ldd unresolved scan                          PASS (0 unresolved)
Provider --check-only --wiring-check-only              PASS
```

The repository build-record validator also returned `status=PASS` with
`ndnsf-local-sif-build-v3`, `containerNativeBuild=true`, and the record-bound
SIF digest (metadata-only mode; the SIF hash is the recorded SHA-256 above).

The ambient `PATH` on the verification host resolves `/usr/local/bin/apptainer`
1.3.4. That binary was not used for the build or probes; future candidates
must record and invoke the qualified 1.5.3 path explicitly.

The Provider check used the read-only r23 D2b bundle, entered its bundle
directory before launch, and emitted `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK` with
two roles and two artifacts.  This is a wiring/plan check; it does not replace
the real CPU/MiniNDN request lifecycle or GPU acceptance gates.

Runtime hashes observed inside the SIF:

```text
di-native-provider                         c8307d73e38299dfbfe88a961082037dada199edf3701af4da91ba62166b9b2e
libndn-service-framework.so.0.1.0          45d55972f9c5471b288ee4ece81c2ba228f5c9601e67ecf0c5814122f8ded138
libndn-svs.so.0.1.0                        1c58ef3d3f7bc028d2b55a66275593ef1b4c21a03fe21328ab1391e3023f80ef
ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
                                             50f73508b262f0ee82ba8453eadabd3f7ff0374770920d541406421ad60f2c00
```

## Promotion boundary

TigerCluster is not involved in this local verification.  Promotion remains
blocked until the candidate record, complete library-lock closure, local
C++/Python/real CPU-MiniNDN functional gates, bundle hashes, and staged
SIF hash are recorded.  After promotion Tiger may only verify the hash, stage
the SIF once, and execute it.

The following historical failures are hard preflight stops for the next
candidate: host-built `_ndnsf.so`/missing `Python.h` (`WRONG_BUILD_BOUNDARY`),
one Waf target missing `NDN_CXX`/`NDN_SVS`/ONNX Runtime closure, stale base
extensions, Provider launches without `cd "$BUNDLE"`, shared HOME/PIB races,
stopped Face loops during large fetches, host Linuxbrew linker contamination
(`HOST_TOOLCHAIN_CONTAMINATION`), and duplicate multi-gigabyte SIF/model
copies.
