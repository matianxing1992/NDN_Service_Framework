# Normal two-node rank orchestration

Date: 2026-09-07. Status: IMPLEMENTED, runtime qualification NOT_RUN.

The private batch supports normal two-node GPU with two tasks and one shared
run/candidate/job-bound probe nonce. Rank zero provisions the existing signed
application inputs exactly once and publishes the authenticated preparation
digest. Rank one waits on the shared record within a monotonic budget and fails
on a recorded rank-zero preparation failure. Both verify the same preparation,
capture real Slurm task identity and bind their own visible device. Endpoints are
derived from attested Slurm hostnames using bounded getent queries; ambiguous,
duplicate, loopback, unspecified and multicast addresses are rejected.

Each rank owns only its assigned homes and separate node output/NFD path. Existing
startup/completion barriers receive the shared nonce. Rank zero validates all four
requests through the existing graph-reference and numerical owners; rank one
does not invoke User. After clean srun exit the batch joins actual retained
node-receipt files and produces the normal collection handoff. Existing native
launch/NDN/CUDA/readiness owners are reused, not replaced by batch-script logic.

The real handoff join exposed a prior integration error: publish_normal_handoff
read plan.candidateDigest, but the actual prepared plan contains no such field
(the plan participates in calculating that digest). Its old test fixture added a
fictional field. The writer now uses its explicit runtime_candidate_digest to
bind both node receipts and collection candidate; tests use the real plan shape.

Full four-request permission+deadline budgets plus stage/start/cleanup require
630 seconds under this profile. The old 600-second walltime was insufficient;
the profile now requests 900 seconds. No schedule or deadline was reduced.

Verification:

```bash
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_distributed_runner.py \
  Experiments/TigerCluster/tests/test_yolo_collection.py \
  Experiments/TigerCluster/tests/test_yolo_local_execution.py \
  Experiments/TigerCluster/tests/test_yolo_allocated_runner.py -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-two-node-runner-20260907/focused.xml
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_allocated_runner.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-two-node-runner-20260907/cli.xml
```

Results: **41 passed in 5.83s**, then **53 passed in 15.74s** (83 unique tests with
latest passing evidence). An earlier single/local boundary check had 25 passes
in 2.88s. New concurrent tests launch two Python threads with explicit Slurm,
issuer and native-rank doubles, exercising shared publication, assigned homes,
same nonce, four-request coverage, first failure and budget rejection. The final
join uses actual retained receipt files and the real handoff writer; allocation
and GPU files remain fixtures and do not prove live runtime behavior.

Remaining: remote staging/relocation, real scratch/capacity integration, negative
dependency and allocation-terminal reconciliation, followed by T007 audit and
formal gates. No Slurm job or GPU/model execution was launched. Another client's
build replaced old PID 1291688 with PID 1339634 and was compiling NAC-ABE when
observed; neither completion nor failure is inferred from the old PID disappearing.

The actual profile was rendered after final source/contract changes. Public CLI
dispatch check exited 78 with integrity VERIFIED, qualification NOT_EVALUATED;
retained JSON: `Experiments/TigerCluster/results/spec183-two-node-runner-20260907/profile-check.json`.
E=`sha256:03c88719b614e93a7015d50dd58268d7e7999c84edff6e23465f6fb13cd517ac`,
profile=`sha256:c0772c522141eb3ffdd52da336b815c12d09fa73ac1b71178b40dd6fc94bcd22`.
I/R unchanged; this is content verification only. CodeGraph sync and active
Context Mode health passed; repository Spec Kit evidence/handoff remain authority.
