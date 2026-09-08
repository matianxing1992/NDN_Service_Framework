# T004.j — external terminal observer

2026-09-07. IMPLEMENTED / component verification, not a Slurm/GPU PASS.

The actual public CLI now forwards `collect --reconcile` into its frozen copy.
It performs existing retained collection and, outside a job only, queries sacct
and squeue through `runtime/yolo_allocation.py`. The real SubmissionJournal only
closes after bound terminal observations have been retained. Numeric collection,
scheduler outcome and bookkeeping remain separate. Default collect is offline.
GPU prerequisite reuse now also verifies successful allocation termination.

Result root: `Experiments/TigerCluster/results/spec183-terminal-20260907/`.

| Result | Scope |
| --- | --- |
| focused.xml: 78 passed, 31.99 s | Terminal mutations/query argv, real journal/receipt, unchanged CLI and prerequisite boundaries |
| gate-boundary.xml: 4 passed, 1 failed | Real prior GPU gate rejects missing/failed/wrong-job terminal records; new frozen-entry fixture lacked harnessManifestSha256 |
| frozen-entry.xml: 1 passed, 0.27 s | Add the required prepared fixture field; real frozen command forwards --reconcile |

Scheduler subprocesses and numerical collector are explicit doubles. The journal
and retained-file checks are real; these tests never allocated a GPU or ran a
model. A read-only historical sacct query on iTiger accepted the selected format
but returned no record for job 119; it is not terminal proof. A bounded historical
query returned jobs 209469/209470/209471 as FAILED, with the expected seven fields
(UID 64102, bigTiger, itiger), but empty comments. These unrelated records cannot
qualify any Spec183 submission. Query failures or
empty accounting leave the journal reserved. The timeout test also confirms
that a scheduler failure does not create a model collection-failure artifact.

Still incomplete: remote transfer/portable prerequisite transport, the negative
case, unknown-submission query recovery and actual site qualification. The SIF
read-integrity failure prevents freezing a trustworthy new actual candidate;
see input-read-integrity.md. T004 and T007 are not closed.
