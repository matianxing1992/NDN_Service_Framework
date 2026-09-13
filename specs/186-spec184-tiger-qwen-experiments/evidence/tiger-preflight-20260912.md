# Spec186 TigerCluster compute preflight

**Captured:** 2026-09-12 (America/Chicago)
**Account:** `tma1`
**Scope:** T009.a allocation and runtime-capability boundary only

The read-only login probe and bounded `srun` probes were run through the
configured `itiger` SSH host. The immutable Spec186 application bundle was
later staged to project storage and verified by digest; no Spec186 SIF was
opened and no application or scheduler campaign was submitted.

| Check | Observation | Result |
| --- | --- | --- |
| Login host | `itiger`, Linux `5.14.0-427.28.1.el9_4.x86_64` | PASS |
| Slurm | `/usr/bin/sbatch`, Slurm `24.05.2`; `bigTiger` is up with H100, RTX 6000 and RTX 5000 nodes | PASS |
| Project storage | `/project` 900T total, 838T free; `/home` 10T total, 7.3T free | PASS |
| Staged application bundle | `/project/tma1/ndnsf-di/apps/spec186/spec186-app-bundle-r4`; 9 files, 215,347,565 bytes; content digest `badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d`; remote manifest recomputation matched | PASS (staging only) |
| GPU allocation | `srun --partition=bigTiger --gres=gpu:1` allocated `itiger05` | PASS |
| GPU device | NVIDIA RTX 6000 Ada, `GPU-eaf346e3-e220-8633-e374-f6595d1e2ff8`, 49140 MiB total / 48482 MiB free, driver `560.28.03` | PASS |
| Compute Apptainer binary | `/usr/bin/apptainer`; `apptainer --version` reports `1.5.3-1.el9` on `itiger05` | PASS |
| Apptainer subcommand | `apptainer version` on `itiger05` did not return within 5 seconds; login host reports `1.3.4-1.el9` but is not a SIF execution host | BOUNDARY |

The compute-node `--version` form is the usable version probe for the next
bounded job. The hanging `version` subcommand is retained as an operational
failure and must not be used in a job readiness loop. Login and compute
The login-node package is outside the NDNSF-DI runtime path. Local SIF build
and inspection use only the installed Apptainer 1.5.3; any SIF must be built
or validated with the compute-node 1.5.3 binary that will execute it. The
cached Spec183 SIF remains ineligible because its source seal and compression
format do not match this candidate/runtime.

The staged app bundle was intentionally probed against the existing
`spec180-runtime-b6710fd6/spec180-runtime.sif` only as a negative compatibility
check. On compute node `itiger04`, `/app/bundle/bin/di-native-provider --help`
exited `127` because the old image lacks `libboost_system.so.1.71.0`. This is
evidence that the old SIF cannot host the Spec186 app layer; it is not a
Spec186 runtime result and must not be promoted.

Reproduction commands:

```bash
ssh itiger "sinfo -h -o '%P|%a|%l|%D|%G'"
ssh itiger "srun --partition=bigTiger --nodes=1 --ntasks=1 --cpus-per-task=1 --mem=1G --gres=gpu:1 --time=00:00:30 --immediate=20 hostname"
ssh itiger "srun --partition=bigTiger --nodes=1 --ntasks=1 --cpus-per-task=1 --mem=1G --gres=gpu:1 --time=00:00:30 --immediate=20 /usr/bin/nvidia-smi --query-gpu=name,uuid,memory.total,memory.free,driver_version --format=csv,noheader"
ssh itiger "srun --partition=bigTiger --nodes=1 --ntasks=1 --cpus-per-task=1 --mem=1G --gres=gpu:1 --time=00:00:30 --immediate=20 timeout 5s /usr/bin/apptainer --version"
```

This closes only the allocation/tool-capability portion of T009. It does not
qualify CUDA model execution, the native Merge CPU role, a numerical oracle,
NDN protocol completion, or cleanup.

## Preflight repair

`Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-compute.sh`
now uses `timeout 10s apptainer --version` as its authoritative compute-node
probe and bounds `nvidia-smi` to 15 seconds. A fake-command regression run
verified that the script writes both evidence files and exits successfully
without invoking the hanging `apptainer version` form.
