# Spec 111 Completion Hold

Date: 2026-07-14  
Verdict: **HOLD — do not claim Spec 111 complete**

The frozen 2026-07-14 MiniNDN campaign exposed startup/import failures before
measurement. Its 20 one-shot cells are retained as immutable diagnostic
evidence, but they do not satisfy T187 and cannot support T188, T189 or T201.

The immediate remediation scope is T202-T207:

- establish `app_sdk.controller` as the sole defining owner of
  `APPController` while retaining thin compatibility re-exports;
- migrate every maintained Controller caller to that canonical owner;
- run the real Controller, Provider and User startup-import paths under an
  isolated source-root `PYTHONPATH` before a campaign output directory exists;
- fail closed when any role cannot import, without constructing NDN faces,
  starting MiniNDN, or creating campaign output;
- preserve the old campaign and correct its failure attribution without
  changing any observed result.

Until a new candidate passes all later T182-T200 gates, the feature status
remains completion hold. No completion summary is present, no compatibility
deletion is permitted, and no performance claim is admissible.

Execution exclusions for this remediation: no MiniNDN performance matrix, no
Docker/OCI/SIF build or runtime, no Apptainer, and no Slurm/iTiger job.
