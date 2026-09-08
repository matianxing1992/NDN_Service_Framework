# T004.k — shared receiver submit and acknowledgment recovery

2026-09-07. IMPLEMENTED, component verification only. T004 remains incomplete;
T007 is not PASS. No sbatch, GPU/model execution, SIF rebuild or download occurred.

The public submit action now reaches its receiver-side owner when the original
prepared paths already exist in the declared shared namespace. Existing content,
profile, frozen harness and typed prior gates still run first. Bad/unstaged paths
do not query the scheduler or reserve a journal. Local-run relocation and upload
are not implemented or bypassed.

The receiver verifies the live cluster, available capacity and batch Python
dependencies. It stores exact submission intent, atomically claims SUBMITTING,
calls sbatch once with no requeue, and binds the parsed acknowledgment. Timeout
or ambiguous output stays uncertain. Retries query sacct/squeue by the original
unique comment and never call sbatch again. A single matching ID reconnects;
empty queries or duplicate IDs keep the reservation. The batch waits within a
finite budget for the submitter's acknowledgment and cannot invent its own.

## Bounded evidence

Results: `Experiments/TigerCluster/results/spec183-shared-submit-20260907/`.

| Artifact | Result / scope |
| --- | --- |
| focused.xml | 85 passed, 18.12 s: shared submit, existing journal transitions, public CLI and allocation boundary |
| receiver-final.xml | 14 passed: final interpreter preflight plus affected receiver cases |
| receiver-ordering.xml | 15 passed, 1.44 s: also reject non-executable wrapper before any scheduler query or reservation |

These tests use real temporary files and journals. Scheduler commands, prerequisite
qualification and the full profile are explicit doubles, not physical deployment
proof. They cover exact-once dispatch, timeout/ambiguous acknowledgments, empty
and duplicate recovery, late acknowledgment after UNKNOWN, path/key/site rejection
before reservation, and batch acknowledgment ordering.

Read-only site observations: scontrol show config reports ClusterName=itiger.
The actual `/usr/bin/python3 -c 'import ... jsonschema ...'` fails with
ModuleNotFoundError: No module named 'jsonschema'. This is an unresolved operating
dependency, now rejected before sbatch rather than discovered in a failed job.
The check does not claim that NumPy or the remaining pins were verified after
that first import failed. A maintained operator environment must be installed
and its interpreter consistently selected before real submission.

Next: resolve operator environment and portable file/evidence transport; connect
the registered negative case. Preserve the existing base-SIF read-integrity
failure and original lock. Do not repeat model tests to work around these gaps.
