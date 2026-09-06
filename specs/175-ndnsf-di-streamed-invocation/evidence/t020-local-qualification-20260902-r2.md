# T020 local qualification — candidate r2

**Status**: PASS for G0-G2; G3 is the next gate

This candidate includes the fail-closed root preflight in
`scripts/run_spec175_g3_matrix.py` and the matching tracked profile hash.

| Gate | Evidence | Result |
|---|---|---|
| G0 | `results/spec175/current-20260902-r2/g0/source-seal.json` and `qualification-manifest.json` | PASS; blocker count 0 |
| G1 | `results/spec175/current-20260902-r2/g1-unit.log`, `g1-python.log`, and Python manifest | native unit: no errors; Python: 375 passed, 1 non-blocking skip, 0 failed |
| G2 | `results/spec175/current-20260902-r2/g2/qualification-manifest.json` | PASS; all registered I-cases present, no failed case |

The source seal and all manifests bind the same candidate. No SIF was built,
uploaded, or submitted. The first valid next action is one invocation of the
checked-in G3 matrix under `sudo -E`; a failure stops the route.
