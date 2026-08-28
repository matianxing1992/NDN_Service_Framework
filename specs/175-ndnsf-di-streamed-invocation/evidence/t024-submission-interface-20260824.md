# T024 submission interface — 2026-08-24

Added the frozen `submit.sh` gate selector for `control`, `stage-readiness`,
`multi-provider`, `conversation-residency`, and `performance`, plus matching Slurm wrappers and the
shared `run-streamed-generation.sh`.  Submission requires a PASS/PROMOTABLE
G0-G4 closure manifest, exact local SIF digest, frozen workload, external
content-addressed model manifest, and remote SIF/model identities before
`sbatch` is reached.  The runtime wrapper verifies the SIF hash and launches
inside the exact SIF with `cd /bundle`; it does not build, materialize, or
overlay Docker/OCI/runtime source.

The interface is implemented and shell-syntax checked.  It is intentionally
not executable as a live submission yet because G0-G4 and the stateful Qwen
subject are still open.

The repository-owned `ndnsf-di-pre-tiger-checklist` validator now runs before
any `sbatch`, SSH, upload, or allocation. It binds the selected positional gate
to the exact candidate ID, local SIF path/digest, and required evidence hashes.
The no-GPU `control` and three-GPU `conversation-residency` wrappers are
tracked, but neither is marked accepted until the exact final SIF exists.

The exact r28 SIF wrapper was also exercised.  The native Provider returns
nonzero for the unsupported `--help` flag while printing its usage contract to
stderr; the wrapper now captures that output and requires the expected
nonzero status plus `--plan` and `--execution-policy`.  The captured usage was
762 bytes.  This is a CLI/working-directory check, not a streamed generation
result.
