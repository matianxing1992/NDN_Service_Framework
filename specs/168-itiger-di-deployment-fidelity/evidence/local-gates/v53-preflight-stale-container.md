# Spec 168 v53 Gate B - stale diagnostic container preflight rejection

## Verdict

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `1178.451 ms`; no Request or
NDNSF runtime attempt began and `automaticRetry=false`.

## Identity and evidence

- Candidate: `20260804T083303Z-v53-native-parser-parity`
- Source identity: `sha256:1d8dcbea19b8748a630bf051e0f9873a4b90ee461d746012919ec6cf64e95d87`
- Native ABI bundle: `sha256:fd920b757ab39c57703254d2c4438e21a9b7753507729ad306c6c4155acb8fd2`
- Gate command: `sha256:073a1cffad2f68c1d94c5f7d3bc55eecb6fc8a0edaf5d6594a8644975688c417`
- Gate manifest: `sha256:d4d26bbbefbd501baa9058337ff5e28bc93db97ddd57162b946f4a5ef33e5320`
- Gate checkpoint: `sha256:1d3e101479bb26100f708f286918ca15d5b18e1848b38f925b0f30694b539ace`

The preflight found five NFD processes owned by the still-running
`spec168-v52-minindn` container. The v52 hard deadline had terminated the
foreground `docker run` client but not the privileged host-cgroup container.
Stopping that exact container removed the NFD and application process tree.

v54 keeps the same native ABI bundle and adds a launcher-owned TERM/EXIT trap
that stops only its exact container name. This is deployment-harness cleanup;
it does not alter NDNSF, the workload, or the model.
