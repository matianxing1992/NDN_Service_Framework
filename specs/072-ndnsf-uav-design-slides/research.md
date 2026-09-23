# Research Notes: NDNSF-UAV Design Slides

## Implementation Evidence

- `DroneServiceContainer` registers provider-specific video, recording,
  catalog, parameter, parameter-edit, preflight, analyze, Targeted MAVLink,
  telemetry, camera-frame, and mission services.
- `GroundStationServiceContainer` owns selected-drone state, command safety
  gates, operator authority leases, mission workflows, recording playback,
  video fetch/adaptation/FEC, and the object-detection provider.
- `FlightControllerBackend` has mock and UDP/serial implementations; MAVLink
  bytes are built on the ground-station side and forwarded by the drone.
- `VideoPublisher::publishCurrentFrame` maps H264 chunks into stream metadata
  and publishes one XOR parity symbol by default.
- `requestVideoLane`, `VideoAdaptivePolicyInput`, and
  `VideoAdaptivePolicyDecision` implement consumer-side RTT/pressure adaptation.
- `attemptAndRecoverFrame` recovers exactly one missing data shard when parity
  is available.
- Drone recording uses an embedded `RepoCore`, hybrid AES-256-GCM-at-rest
  content, a manifest, and exact named chunk retrieval.
- `Experiments/NDNSF_UAV_GUI_Minindn.py` provides headless and GUI smoke paths
  for control, telemetry, mission, video, recording, parameters, preflight,
  authority, and operator-dashboard behavior.
