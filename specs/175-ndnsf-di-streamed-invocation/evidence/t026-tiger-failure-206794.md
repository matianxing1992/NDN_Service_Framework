# T026 Tiger submission failure — 206794

**Date:** 2026-08-28  
**Candidate:** `spec175-final-candidate-replay42c`  
**Gate:** `multi-provider`  
**Result:** **FAIL before SIF execution**

The functional bundle, Qwen stage manifest, candidate/SIF binding, and all
pre-submit checks passed before submission.  The positional
`submit.sh multi-provider` command created Slurm job `206794`, which was
scheduled on `itiger02` and exited in `00:00:00` with code `1:0`.

The batch stderr contains only:

```text
/var/spool/slurmd/job206794/slurm_script: line 17:
SPEC175_REMOTE_MODEL_ROOT: set SPEC175_REMOTE_MODEL_ROOT to the external model root
```

The submission script validated `REMOTE_MODEL_ROOT` but exported the distinct
name `SPEC175_REMOTE_MODEL_ROOT` without assigning it.  Consequently the
`set -u` expansion in `qualify-multiprovider.sbatch` stopped before
`run-streamed-generation.sh`, before Apptainer, before NFD, and before any
NDNSF-DI Provider or model code ran.  This is a submission-environment defect,
not a SIF, CUDA, ONNX Runtime, or NDNSF-DI functional result.

The remote Slurm records and failed output are retained under:

```text
/project/tma1/ndnsf-di/staging/spec175/replay42c/remote-submit-r1/
```

The fix assigns `SPEC175_REMOTE_MODEL_ROOT="$REMOTE_MODEL_ROOT"` and adds a
regression assertion.  This failure remains part of the T026 audit trail and
does not count as a functional invocation or as evidence for G6.
