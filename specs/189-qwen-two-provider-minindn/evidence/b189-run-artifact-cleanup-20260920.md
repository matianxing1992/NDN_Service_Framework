# Spec189 Local Run-Artifact Cleanup

## Scope

2026-09-20 local evidence maintenance after checkpoint `d13d6045` was pushed
to `origin/Experimental`.

The cleanup target was restricted to the local directory
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/`. It removed 104 old
run-scoped directories whose names were `encrypted-repo`, `canonical-repo`, or
Provider `cache`. The complete
`two-provider-global-r139/` run root was explicitly excluded and retained.

## Preserved evidence

- Old run `*.log`, JSON, certificate, and resource-sample files remained.
- The complete r139 run root remained available for the next changed gate.
- No files under `/home/tianxing/.codex` were touched.
- No Git refs, Git history, Codex turn-diff refs, or conversation files were
  modified.

## Result

Before cleanup, the selected old cache/repository directories occupied about
`41 GiB`. After cleanup:

- root filesystem free space increased from about `4.1 GiB` to `42 GiB`;
- the full Spec189 `runs/` directory is about `4.9 GiB`;
- the retained r139 run root is about `4.5 GiB`;
- no excluded cache directory remained outside r139.

This is evidence maintenance only. It does not promote any MiniNDN stage,
change task status, or alter the r139 `RESOURCE_BOUNDARY:diskFree` result.
