# Spec176 baseline evidence

Date: 2026-08-28 (post-audit rebuild)
Branch: `UAV-Experimental`
Source revision observed: `37f8a468a3b280ab4ca623cffba2f1d58b63a093`

This is the CPU/in-process baseline for the application contract. It is not
MiniNDN, SITL, hardware, or a performance result.

## Build

The focused and UAV application targets were built with the repository's current system toolchain:

```text
PATH=/usr/bin:/bin:/usr/local/bin:$PATH
AS=/usr/bin/x86_64-linux-gnu-as
LD=/usr/bin/x86_64-linux-gnu-ld
CXXFLAGS=-O0 -g0 -fno-inline
./waf build --targets=unit-tests,integration-tests -j1
./waf build --targets=UavDroneApp,UavGroundStationApp -j1
```

The build completed successfully. The resulting binary hashes were:

```text
24715ab2e5b67b5ed739b48ab055d8b6bd2d08e0e3df927f7576b63e9db24bc9  build/unit-tests
e8051957fe6097f01d5c5080675f9a784189534acfe3ce990a9a0de64cef170c  build/integration-tests
8386c8449c288aab48383535b89a33fb2824dce31ff5623e45505c6e70f9b38d  build/examples/UavDroneApp
939324f19c2794047b54243da7073990a1cdc4f3c6ad1549c47e2d516b703457  build/examples/UavGroundStationApp
62af5902499f23f9c84f08c2772a303d899decc4bf4ddef55aac87a4569233ac  build/examples/App_ServiceController
```

## Focused behavior (recorded before final selector wiring)

```text
./build/unit-tests --run_test=UavTwoLifecycle --log_level=test_suite
  11/11 cases passed
./build/integration-tests --run_test=UavCollaborationFlow --log_level=test_suite
  3/3 cases passed
python3 tests/python/test_uav_collaboration_campaign.py
  2/2 cases passed
python3 examples/ndnsf/uav-collaboration/minindn_uav_collaboration.py
  preflight passed; MiniNDN and all required binaries/configuration were present
python3 examples/ndnsf/uav-collaboration/minindn_failure_matrix.py --run \
  --window-seconds 8 --output /tmp/spec176-failure-matrix4.FAmMYX
  10/10 frozen failure cases passed
python3 examples/ndnsf/uav-collaboration/minindn_uav_collaboration.py --run \
  --window-seconds 60 --output /tmp/spec176-minindn-stream60b.jpROqE
nominal four-process run passed; stream and second-consumer evidence recorded
```

The historical focused logs above preserve the assertion counts from that
run; they are superseded for process-level status by the current reruns below.
The current candidate's two repeated unit and integration invocations each
returned 0, as recorded in `evidence/cpu-lifecycle.md`.

The cases cover separate MissionSession/CollaborationJob lifetimes, bounded
state transitions and snapshot recovery, contextual producer-owned names,
capability-first detector selection, explicit fallback, signed exact-name Data
verification, tamper rejection, and correlation/endpoint diagnostics.

The integration lifecycle follow-up also covers a two-part mission with
independent video/telemetry bindings: a finite request times out after its
deadline while the MissionSession remains active and completed mission work is
preserved.

The focused tests do not close T020: the real multi-process MiniNDN nominal and
failure-matrix gates are recorded separately, and PX4 SITL remains a separate
acceptance requirement.
The current UAV application build proves linkage only; it does not prove a
networked collaboration by itself. The Drone collaboration service now uses
the selected participant's validator-backed exact-Interest fetch, and the
T018/T019 multi-process evidence is recorded separately.
The launcher now starts the controller with the UAV policy/trust-schema and
uses `--runtime-config`. The post-selector nominal 60-second MiniNDN run and
ten-case failure matrix are recorded in the corresponding evidence files; the
remaining SITL gate is intentionally not inferred from them.

## Selector-wiring follow-up (2026-08-28)

After the Ground Station selector was connected to ACK capability metadata, the
candidate was rebuilt successfully. An earlier diagnostic invocation exposed a
host-library static-teardown/KeyChain failure, but two subsequent focused unit
and integration reruns each returned 0 with all assertions passing. The Python
analyzer passed (2/2). An initial fresh MiniNDN launch stopped before the
controller-ready marker because `App_ServiceController` crashed while
`CertificatePublisher::findCertificate` handled the current system ndn-cxx
exception/RTTI path. The controller identity path was then corrected, and the
candidate-consistent nominal and failure-matrix reruns are recorded in the
dedicated evidence sections.
