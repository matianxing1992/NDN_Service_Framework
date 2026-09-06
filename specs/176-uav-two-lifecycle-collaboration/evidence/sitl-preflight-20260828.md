# Spec176 PX4 SITL preflight

Date: 2026-08-28
Branch: `UAV-Experimental`

Command:

```text
PX4_SITL_ROOT=/home/tianxing/PX4-Autopilot \
NDNSF_UAV_FLIGHT_CONTROLLER=udp \
bash NDNSF-UAV-APP/tools/run_uav_collaboration_probe.sh \
  /tmp/spec176-sitl-preflight
```

The probe correctly failed closed before launching a scenario:

```text
SPEC176_SITL_BLOCKED reason=scenario-missing \
path=/home/tianxing/PX4-Autopilot/Tools/sitl/run_uav_sitl.sh
```

`PX4-Autopilot` is present and the UAV binaries are built, but this repository
does not contain the configured `run_uav_sitl.sh` scenario adapter. No PX4 SITL
success or hardware claim is made. T020 remains open until a real scenario
adapter is provided and the two-lifecycle patrol/stream/incident/recovery
acceptance is executed.
