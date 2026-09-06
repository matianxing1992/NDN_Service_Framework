# NDNSF Project Context Anchor

This is the stable project-level Context Mode authority for the NDNSF
workspace. It is intentionally independent of `.specify/feature.json` and
active Spec selection.

Project root:

```text
/home/tianxing/NDN/ndn-service-framework
```

The project layer may retrieve durable cross-Spec architecture, API, design,
and historical decision documents. It must not be used as authority for the
current task's active checkpoint, blockers, or acceptance status.

The active-Spec layer remains separate and is selected by
`.specify/feature.json`. Queries about the current implementation plan,
tasks, contracts, or evidence must use the exact active-Spec source label and
pass its freshness checks.

Codex and Claude Code use separate Context Mode stores. Both stores index this
same file-backed anchor and the same repository documents; neither store is a
shared project database and neither client may delete the other's store.
