# T023 G4 failure and correction: relative case output path (2026-09-01)

The first exact-SIF replay of the repaired candidate reached the real MiniNDN
topology and started the controller, repository, and four Provider processes.
The User then exited before publishing the business Request.  The user log
contained:

```text
RuntimeJournalUnsafeRootError: runtime journal state root is not safely writable
OSError: [Errno 30] Read-only file system: 'results'
```

The host replay driver passed `results/spec175/g4/.../M01-r1` as a relative
`--output-dir`.  SIF-owned processes start with `/opt/ndnsf-di/replay/repo` as
their working directory, so the journal path became an image-local `results`
directory instead of the bind-mounted host case directory.  This is a harness
path defect, not a model, ONNX Runtime, or NDNSF protocol result.

`replay-exact-sif.py` now resolves the aggregate manifest before deriving case
directories and always passes absolute host paths to the production runner.
The regression `test_exact_replay_resolves_case_output_for_sif_bind_mount`
locks this requirement.  The interrupted replay is retained under
`results/spec175/g4/exact-current-v3-boundary-fix-20260901/` as diagnostic
evidence and is not pooled as G4 evidence.  Because the replay driver is part
of the sealed runtime/replay subject, the corrected path requires a fresh G0,
G3, SIF build, and G4 sequence; no old SIF or partial replay may be reused.
