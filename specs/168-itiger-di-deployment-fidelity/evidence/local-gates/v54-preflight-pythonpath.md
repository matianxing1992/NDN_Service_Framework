# Spec 168 v54 Gate B - overlay import path preflight rejection

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `2456.339 ms`; no NDNSF
Request began. Native ABI closure and full overlay import passed, then the
frozen source harness failed to import `NDNSF_NewAPI_Minindn_Perf` because the
v41-derived launcher omitted `/workspace/Experiments` from `PYTHONPATH`.

- Source identity: `sha256:255326e01b078dda1f3bf7aa744657e53e5a96ac5e4ad70fbddd8ac733facdbe`
- Gate command: `sha256:f9ab0f0b883c4459aa29d206861f8b66d050a89622a9515fad9d29cb6754c312`
- Gate manifest: `sha256:cafc632d5cb3a394f214b9c50a8b310a67ea7795208c2e34c92672eebba165ab`
- Gate checkpoint: `sha256:871f7e119e11b9ea8736564f62122a36e5d78569919af04852ba2450c23dcb19`

The exact-container cleanup trap ran successfully: no v54 container, NFD or
NLSR remained. v55 retains that trap and the same native ABI bundle, while
adding the already-established v49 Python search path.
