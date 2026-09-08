# T004 / T012 runtime-version enforcement — 2026-09-08

**Scope**: T007 N3 source repair and focused boundary acceptance.
**Status**: IMPLEMENTED / focused checks PASS; no SIF, MiniNDN or GPU qualification.
Source parent: `cc638d00`. Parent progress remains 2/17.

`runtime.apptainerVersion` now reaches an actual bounded `apptainer --version`
command in `provision_run` before the issuer container and in `run_normal_node`
before any rank workload, including local CPU and the two-rank negative case.
The latter uses `NodeRuntime.verify_runtime`; prepared workers also reject every
service/finite-role launch before verification or after changing the runtime
path/version. The low-level direct constructor remains a component test seam.

Both call the same `verify_runtime_version` owner in `runtime/yolo_worker.py`.
It reuses `run_finite_application` and `Processes`, bounds the command by the
smaller of ten seconds and the remaining stage budget, and applies the existing
cleanup budget. Exit failure, mismatch, timeout, or forced/unreaped cleanup
cannot authorize a container. The one-attempt directory is exclusive; repeated
calls do not overwrite evidence or rerun the command under the same identity.

The issuer keeps `prepare-output/runtime-version/{receipt.json,version.log}`;
each rank keeps `nodeN/runtime-version/{receipt.json,version.log}`. Records bind
run/candidate/rank, exact command, expected version, raw log hash and actual
finite-child cleanup. Log and receipt permissions are explicitly 0600.

Public `_reanalyze_retained` requires the issuer and every expected node record
for normal and negative verdicts. It rechecks the exact version output, exit0,
reaped/non-forced cleanup and external run/candidate/rank against the frozen
effective profile, then retains `runtimeVersions` in the immutable final verdict.
The transport inventory includes those same receipt/log bytes for every reused
prerequisite. Offline collection never invokes Apptainer or hashes a SIF for
this observation. Version identity alone never becomes model/GPU qualification.

## Retained verification

Raw outputs: `Experiments/TigerCluster/results/spec183-runtime-version-20260908/`.
All commands use the existing local `python3 -m pytest -q`; no new environment.

| File | Scope | Result |
| --- | --- | --- |
| `launch.xml` / `.log` | New version reader/process checks, issuer, prepared worker and node orchestration | 37 passed, 4.889 s |
| `consumers.xml` / `.log` | Public submit, transport inventory, worker, operator, application, allocated runner | 148 passed, 11 failed, 20.118 s; preserved |
| `repairs.xml` / `.log` | Changed version/application/transport modules after corrections | 78 passed, 6.652 s |
| `final-boundaries.xml` / `.log` | Issuer/rank wrong-version/exit/timeout, normal public reanalysis, four case owners refusing version failure | 11 passed, 1.510 s |
| `network-boundary.xml` / `.log` | Existing finite network probe and NFD/nfdc lifecycle fixtures after the required version observation | 8 passed, 4.788 s |

The sets overlap; do not add their totals. The consumer run exposed two transport
fixture failures from umask-derived modes, seven application fixtures that set
preparation directly without the new version step, and two old worker doubles
without `verify_runtime`. Correct fixture setup and explicitly create the real
version log with 0600 before the shared process owner opens it. Do not weaken the
production gate to preserve obsolete fixture behavior. Public negative collection
also rejects missing, failed-cleanup and other-run version records while preserving
an already-written verdict.

No native compilation, full SIF hash, upload, model inference, MiniNDN or Slurm
operation was performed. The actual expected compute version remains a later
qualified input; the current profile value was not silently changed to match the
local host. N3's source gap is repaired. N1/N2 host semantic qualification and
the MiniNDN wrapper remain controlling T007 blockers. Final E must freeze these
changed existing harness files and the updated contract; the harness count stays
28 and no native rebuild is triggered solely by this repair.
