# T005 NFD network setup component

2026-09-07. PARTIAL / native NFD, exact SIF and Tiger execution NOT_RUN.

`configure_network` now produces the `nfd-ready` and `routes-ready` records
consumed by startup coordination. It requires a prepared Worker and matching
run/candidate/rank barrier, validates allocation endpoint shape, unique ranks
and addresses, the profile-pinned TCP port, and the application Sync name.
Two-node runs reject loopback/unspecified/multicast endpoints. The final outer
operator must obtain these endpoints from its verified Slurm allocation.

The component starts the existing exact-SIF NFD configuration, waits for an
actual UNIX socket while checking owned processes/peer failures, and executes
in-image `nfdc status report` as a finite owned child. Both ranks must report
the same complete endpoint mapping before face/route changes. Two-node setup
then creates the permanent TCP face, installs the aggregate run namespace
route, sets multicast at **applicationName + `/sync`**, and records face/route
list commands. Single-node setup requires the local management check and Sync
strategy/listing but no remote TCP face. Every command uses the remaining
shared startup budget and a uniquely recorded invocation.

`NodeRuntime.run_management` serially borrows the idle BackboneNeck/DetectShard0
HOME, rather than concurrently using the live NFD's PIB. There is no model or
GPU mount; the HOME lease is released before signed probes and Provider startup.
Nonzero exit, peer failure, socket symlink/wrong type or exhausted budget prevents
route readiness. The final network-setup receipt is exclusive; failed command
argv/exit/cleanup remain in the Worker launch and cleanup records.

The shared `route_commands` helper now accepts the application Sync prefix
explicitly. Legacy `configure_routes` retains its default `/group`; YOLO uses
the prepared appName/sync value instead. Source inspection confirmed the pinned
SVS commit `9f2d8a47cd2a25a5f9ade661c9dbe8acd6416a20` registers `m_syncPrefix`
(ndn-svs/core.cpp:232–239). The local NFD nfdc source sets CHILD_INHERIT by
default (tools/nfdc/rib-module.cpp:151–163); therefore an aggregate namespace
route is not, by itself, evidence of a missing route for more-specific local
registrations. Do not label that a forwarding bug without the exact runtime
RIB/FIB evidence. Actual bidirectional signed probing remains mandatory.

## Verification

Seven new component cases execute real OS child groups and UNIX sockets behind
explicit fake NFD/nfdc boundaries: two-node and local-CPU success, command
failure, non-socket file, profile-port mismatch, duplicate remote address,
and explicit app Sync argument/rejection checks. They verify command sequence,
no model/GPU mounts, separate HOME ownership, clean reaping and no false
routes-ready on failure. Wrong allocation inputs reject before any process.

Full focused suite: **419 passed in 26.61s**. JUnit:
`Experiments/TigerCluster/results/t005-nfd-network-setup/junit.xml`.

This is command/lifecycle wiring evidence, not actual NFD or distributed
inference evidence. The final operator still must build qualified Workers,
call configure_network → start_workload, execute/collect the real requests,
and always close both workers. T005/T006/T007 remain incomplete; no Slurm job
or model run was submitted.
