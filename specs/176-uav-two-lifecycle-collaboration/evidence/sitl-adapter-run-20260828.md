# PX4 adapter trial (diagnostic, not acceptance)

The first root-authorized trial of the new adapter reached a real PX4/jMAVSim
runtime and the predictive stream gate:

```text
SPEC176_SITL_STAGE stage=stream result=true chunks=3
```

It did not reach acceptance. The Python launcher treated
`--auto-spec176-sitl-test` as a single-drone mode, so the Ground Station
reported `Patrol demo needs at least two drones`; the nominal incident then
failed at ACK closure because the detector/source set was incomplete. The
adapter correctly returned nonzero and preserved the full candidate-bound
logs under `/tmp/spec176-uav-sitl-adapter-run`.

The launcher has since been corrected to include the Spec176 mode in its
multi-drone selection and readiness paths, and `drone-C.conf` was added for the
compute UAV. A replacement root-authorized run is still required; this file is
kept as diagnostic evidence and is not counted as T020 success.

## Candidate-bound acceptance rerun (r14, 2026-08-28)

The official wrapper was then run against the same candidate from a rootless
user namespace, with PX4/jMAVSim and UDP-backed flight-controller instances for
three drones. The command was:

```text
timeout 360s unshare -Urnm sh -c '
  mount -t tmpfs tmpfs /tmp
  mkdir -p /run/nfd
  mount -t tmpfs tmpfs /run/nfd
  export PX4_SITL_ROOT=/home/tianxing/PX4-Autopilot
  export NDNSF_UAV_FLIGHT_CONTROLLER=udp
  exec NDNSF-UAV-APP/tools/run_uav_collaboration_probe.sh \
    results/spec176-rootless-sitl-r14-20260828'
```

The wrapper returned code `0`. Its candidate-bound summary is
`results/spec176-rootless-sitl-r14-20260828/summary.json`, with
`status=PASS`, `missingStageMarkers=[]`, and all seven required markers found
in `gui/ground-station.log`:

```text
SPEC176_SITL_STAGE stage=stream result=true
SPEC176_SITL_STAGE stage=patrol result=true
SPEC176_SITL_STAGE stage=incident-success result=true
SPEC176_SITL_STAGE stage=incident-failure result=true
SPEC176_SITL_STAGE stage=command-reconciliation result=true duplicate_execution=0
SPEC176_SITL_RESULT ok=true stream=true patrol=true incident_success=true incident_failure=true compensation=true reconciliation=true
GS_SPEC176_SITL_EXIT ok=true
```

The manifest records candidate tree `680710283ef1b4a578fcae03da90b282df539a40`,
PX4 tree `b405d75553905d272c0c69aa3e2d29a8e3fc8d0c`, a 60-second measured
window, and the three-drone topology. The low-load stream profile is explicit
in `command.json` (`video-fps=5`, `video-width=320`, zero video FEC parity,
and mapped-live prefetch), preventing bootstrap buffer pressure from being
mistaken for a lifecycle failure. Compensation uses a fresh mission upload
after `MISSION_CLEAR_ALL`; candidate filtering keeps completed providers out of
the compensation attempt. This closes the T020 SITL acceptance contract for
the candidate-bound rootless PX4/jMAVSim scenario. It is not hardware-flight
evidence and does not authorize T022 promotion.

## Clean-candidate rerun (r15, 2026-08-28)

After committing the implementation as
`e9c096420909039c50b726ad9b7417e496dcbc04`, the same official wrapper was
rerun without source changes:

```text
timeout 360s unshare -Urnm sh -c 'mount -t tmpfs tmpfs /tmp; mkdir -p /run/nfd;
mount -t tmpfs tmpfs /run/nfd; export PX4_SITL_ROOT=/home/tianxing/PX4-Autopilot;
export NDNSF_UAV_FLIGHT_CONTROLLER=udp; exec
NDNSF-UAV-APP/tools/run_uav_collaboration_probe.sh
results/spec176-rootless-sitl-r15-20260828'
```

The wrapper returned `SPEC176_SITL_RESULT returncode=0`. The summary at
`results/spec176-rootless-sitl-r15-20260828/summary.json` reports
`status=PASS`, `returnCode=0`, and `missingStageMarkers=[]`. Its preflight binds
the run to commit `e9c096420909039c50b726ad9b7417e496dcbc04` and tree
`0dfe3927af1501172fe5a75cb85c9c7969d261b6`, with PX4 commit
`b405d75553905d272c0c69aa3e2d29a8e3fc8d0c`. The three-drone topology, 60-second
window, named-Data/no-endpoint application contract, patrol compensation,
successful and failed incident jobs, stream independence, and
`duplicate_execution=0` reconciliation marker are present in
`gui/ground-station.log`. This is the final candidate-bound SITL evidence for
T020 and the T022 release audit; it remains simulator evidence, not a hardware
flight claim.
