# Spec 168 v56 Gate B - incomplete source-root preflight rejection

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `3552.215 ms`; no NDNSF
Request began. ABI closure and imports passed, but the bundled harness tried to
execute `/source/examples/.../plan_pipeline.py`, which the minimized source
bundle does not contain.

- Source identity: `sha256:be6cb7dc20a21c08c89e87de74550ffececd9ad11e19c62df1b4e6b2519996ee`
- Gate command: `sha256:fb0605691eb80bbf8e316f7d187811d5e50b80f4cfce9a025925a33a4e541956`
- Gate manifest: `sha256:287b7cd5c9893e60d379aed887a1eb436bebb9f7ab8c613183ef32e66d69f30b`
- Gate checkpoint: `sha256:977d4822875257e920451ef13dc5f7f57e33a710da04879a9861e4f6f5cfbf84`

v57 uses the already-established v49 arrangement: execute the workspace
MiniNDN harness (whose example tree is complete) while overlaying the frozen
current Python and native ABI implementation. Cleanup again left no container
or NDN process.
