# Shared Spec Kit Entrypoint Contract

This exact paragraph is the shared contract block embedded in each command-specific
Spec Kit entrypoint. Entrypoints intentionally retain command-specific instructions;
they are not byte-for-byte copies of this shared skill.

<!-- SPECKIT_SHARED_CONTRACT_START -->
For feature work, use the repository canonical `skills/speckit-code-design/SKILL.md`; for NDNSF-DistributedRepo lifecycle, receipt, status, reuse, or retrieval design, also read `skills/speckit-code-design/references/distributed-repo-design.md`. These shared rules define the `Command Output Contract`, `Design binding`, `Progress Timestamp`, and references including `skills/speckit-code-design/references/batch-quality-gates.md`, `review-agent`, the five-lane `Coverage matrix`, and `project-symbol definition map`. Follow the shared contract without weakening this command's own read/write scope. Verify entrypoint synchronization with `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints`.
<!-- SPECKIT_SHARED_CONTRACT_END -->
