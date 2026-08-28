# Static Container and iTiger Handoff Final Gate

Date: 2026-07-14  
Verdict: **PASS (static only)**

The post-remediation selected unit/contract set ran **80 tests, 0 failures**:
73 direct tests plus seven legacy `tests/container/unit` module tests. Logs:

- `/tmp/spec111-final-r2/container.log`, SHA-256
  `52bf5ba337eb8069661f6532f15d5a977e452059d788bf487c73ccf6ed8947be`
- `/tmp/spec111-final-r2/container-extra.log`, SHA-256
  `8b7f5c4ef618a69614d72c7ca4b00be6630981d023da242072cac3d4947edfc6`

The Spec 111 fixture validates exact-format OCI/SIF/revision/process-map
digests without claiming they exist, revision-derived `/LLM/Prefill` and
`/LLM/Decode` roles, one SIF digest for NFD/controller/deployment/providers/
client, read-only model/artifact/per-role identity binds, identity-partitioned
persistent `/state`, a shared node-run NFD socket, unique Provider GPU UUIDs,
and disjoint scheduler/deployment/request state machines.

Execution inventory for this gate: **0 Docker, 0 Podman, 0 Buildah, 0
Apptainer runtime calls, 0 OCI builds, 0 SIF builds, 0 `sbatch` submissions**.
Only Python unit/contract processes and fixture/render helpers ran. The real
runtime fields in `post-separation-candidate.json` remain
`DEFERRED_TO_SPEC110`.
