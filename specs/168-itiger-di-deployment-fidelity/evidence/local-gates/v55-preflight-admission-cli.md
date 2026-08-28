# Spec 168 v55 Gate B - obsolete admission CLI preflight rejection

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `3513.062 ms`; no NDNSF
Request began. ABI closure and overlay import passed. Current harness rejected
the obsolete `--spec168-admission-output` CLI argument; admission output is now
provided only by `NDNSF_SPEC168_ADMISSION_OUTPUT`, as already done by v49.

- Source identity: `sha256:63b50928f09722f1177247c4523bd6b6bfe649ba5206bed778e4581a1b14c390`
- Gate command: `sha256:05f6b02acb2a97a0485a0b2566f328e8d079d5d9fc8029b8940d0337d8ae469b`
- Gate manifest: `sha256:9a81a271712171f563b318bedba31355b24f8cac0e974aaa60ed15b75c75ec5c`
- Gate checkpoint: `sha256:cdc31d938e7d4e11f13e4b8715c0e9126f8ddac438206b0125b89f66c856a448`

Cleanup again left no container, NFD or NLSR. v56 removes the obsolete CLI
argument and retains the single environment-owned admission path.
