# PX4 SITL preflight after NDNSF-DI pattern alignment

The unit and CPU integration gates were rerun before this check:

```text
./build/unit-tests --run_test=UavTwoLifecycle --log_level=message
15 test cases; return code 0

./build/integration-tests --run_test=UavCollaborationFlow --log_level=message
6 test cases; return code 0
```

The read-only SITL launcher was then invoked with the installed PX4 tree:

```text
PX4_SITL_ROOT=/home/tianxing/PX4-Autopilot \
NDNSF_UAV_FLIGHT_CONTROLLER=udp \
./NDNSF-UAV-APP/tools/run_uav_collaboration_probe.sh \
  /tmp/spec176-sitl-preflight-reference
```

It failed closed with:

```text
SPEC176_SITL_BLOCKED reason=scenario-missing \
path=/home/tianxing/PX4-Autopilot/Tools/sitl/run_uav_sitl.sh
return code 2
```

This is a runner/prerequisite gap, not a passing or failing UAV protocol
result. The PX4 tree provides the standard `Tools/simulation/jmavsim` and
`sitl_multiple_run.sh` entry points, so T020 still needs a candidate-bound
adapter that uses those entry points and emits the required readiness,
patrol, stream, incident, compensation, and reconciliation markers.

The adapter is now present at
`NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py`. A subsequent read-only
preflight on the same candidate found all required UAV/PX4 binaries, the
standard jMAVSim launcher, topology, and `xvfb-run`; it failed closed only
because the current shell is not root (`runningAsRoot=false`). The resulting
manifest records the candidate tree and SHA-256 hashes. This confirms the
runner wiring without counting a non-root preflight as SITL acceptance; an
authorized root run is still required for T020.
