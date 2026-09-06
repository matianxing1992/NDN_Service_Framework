# T024 one-launcher and repeated-failure correction (2026-09-02)

## Answer to the operator question

TigerCluster should be driven by one checked-in launcher and one checked-in
configuration/profile.  A campaign may vary only the parameters explicitly
declared by that profile.  The previous workflow did not satisfy that rule in
practice: several commands shared a job name or a wrapper, but their effective
subject was changed by inherited environment, working directory, staged files,
SIF contents, dependency resolution, or an outer timeout.  Consequently, a
new Slurm job was often a new experiment subject, not a repetition of the
earlier successful run.

The right rule is therefore:

```text
one profile + one launcher + one configuration tree
    -> generated run records for declared rows
    -> one sealed source/runtime/artifact tuple
    -> lower local gates
    -> one exact-SIF replay
    -> one Tiger submission path
```

The run record is data, not a second shell script.  It may select only the
profile's allowlisted identity/output/model deltas (and the explicitly
consumed stage-device mapping where applicable).  Coverage, speed, timeout,
Provider/GPU count, topology, workload, model/runtime, or resource changes are
subject changes; each requires a new profile version, source seal, lower-gate
qualification, and candidate.  They must never be supplied as ambient
environment overrides.

## Why earlier "retries" failed

The following defects were observed in the historical Spec175 attempts:

1. `--export=ALL` and exported names not consumed by the tracked wrapper
   allowed the effective workload to drift without appearing in the record.
2. Relative model/artifact paths were resolved from the wrong `cwd` because the
   Provider launcher did not enter the mounted bundle.
3. A host-built `_ndnsf.so` or a stale SIF crossed the container ABI boundary;
   host import success did not prove SIF import or closure.
4. NDN-SVS headers from the Experimental tree were paired with an older
   `/usr/local` library.  Compilation could succeed while the production
   binary lacked the required catch-up symbol.
5. Source, submit-bundle, SIF, model, and evidence changes were mixed with old
   manifests, so a readable result was mistaken for current evidence.
6. An external campaign timeout stopped a matrix before the driver wrote a
   terminal manifest, and partial rows were treated as a complete campaign.
7. Readiness markers and merged logs did not prove that Controller, repository,
   User, and all four Providers were ready before the first request.

These are control-plane and evidence-boundary failures.  They do not show that
NDNSF logic is intrinsically nondeterministic, and another job number cannot
repair them.

## Required prevention

Before SSH, upload, staging, model transfer, or `sbatch`, the repository gate
must render and hash the complete command and effective environment, verify
source/profile/runner/configuration/SIF/model identity, reject unknown or
unconsumed fields, validate the bundle `cwd` and artifact roots, and run the
negative mutation tests with zero external side effects.  The exact rendered
bytes are then submitted through:

```text
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  <gate> <proven-tiger-profile.json> <run-record.json>
```

The local replay uses the same subject and records one fresh directory per
case.  It requires all four Provider readiness records, a per-case watchdog,
zero surviving owned processes, child exit codes, a terminal case result, and
a terminal campaign manifest whose remaining rows are `NOT_RUN` after the
first incomplete row.  A source/dependency/runner/configuration/evidence
change invalidates the old seal and restarts at the earliest affected gate.

## Evidence status

This note records the control-plane correction only.  It is not a new G0--G3,
SIF, or Tiger qualification result.  The current local frontier remains the
NDN-SVS Experimental header/library parity gate documented in
`evidence/t020-g1-svs-api-parity-20260902.md`.
