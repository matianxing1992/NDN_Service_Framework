# Spec193 large-range design checkpoint

2026-10-07 18:44 -05:00. T002 remains open. A06_LARGE adds a131123B five-host Persistent ONE/A success campaign with an independent Arizona native consumer; smaller2048B/4401B local evidence remains separate.

The initial timeout proposal was blocked by read-only review before build/runtime. Current publishFile already derives its begin deadline from source size; the proposed per-RPC override was unnecessary and its wrong-mode guard followed early side-effecting modes. Removed the entire CPP delta and corrected the contract. CPP exactly equals baseline `ed0880cd…`; recorded P05-built native binary remains `4045e056…`. Corrected runner `011c8ae1…` received five-lane STATIC_PASS; no runtime PASS yet.

The3600s child ceiling bounds a successful campaign, not the native23760s failure/recovery ceiling. Timeout is FAIL. Closure requires complete committed bytes and ONE receipt on A, no writes/state change on B, authenticated A-only range writes/commit, independent consumer SHA and cleanup, twice under one matching frozen lock. More range logs or a longer timeout alone cannot close the case.

Context Mode semantic capture currently fails NO_REAL_SESSION_EVENTS; repository Spec193 pointer/tasks/contracts/evidence and CodeGraph provide the authority fallback. CodeGraph sync/Python AST passed. Spec Kit design/review gate used; Spec task registry preserves resumable state; ARS is not applicable to this functional success test. Source composition/clean reproduction/full matrix remain T007; no push or Tiger qualification.

Local design: `specs/193-distributedrepo-multinode-api-campaign/contracts/large-range-campaign.md`; review: `evidence/a06-large-review-20261007/review.md`. Next: freeze/preflight the corrected composition, run the same candidate twice, then address remaining A19/A21/O17 and T007 qualification.
