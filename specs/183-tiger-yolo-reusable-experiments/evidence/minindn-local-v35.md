# APP v35 exact-SIF local evidence

**Updated 2026-09-10.** This record covers the local process-boundary checks
used to promote APP v35 over the locked v22 base SIF. It is not Tiger GPU
qualification.

## Frozen composition

- Base SIF: 3901079552 bytes,
  `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`.
- APP manifest: `sha256:7c8c994fda2bf20aaf322407e9c6b8a4d1f93a050f6d7782986cd4053fdeb91b`.
- Source revision: `e57273d9e769386c11ff744a8d7f907210cbee25`.
- Candidate planes: v45; profile: `yolo-two-node-controller-v42.json`.

## Results

| Run | Result |
| --- | --- |
| `minindn-local-20260910-v97-v35-yb` | `T010_DONE`, exit 0; normal Y-B request/ACK/Selection/response and cleanup. |
| `minindn-local-20260910-v98-v35-yn` | `T010_DONE`, exit 0; all registered authorization and dependency-negative subcases passed. |
| `host-gate-v35-r2` | Strict host semantic validation PASS; shared receipt `host-minindn-v35.json`, 4510 bytes, `sha256:e4ad9acba7cab6ec1d548b8dfb14be94c550f4a8986575b6bacf1d7d8b45fa6d`. |
| `minindn-local-20260910-v100-v35` / `tiger-local-cpu-v35` | `NORMAL_EXPERIMENT_PASS`; two requests, each `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.0005340576171875`, nine dependency edges, clean cleanup. Shared verdict: 225893 bytes, `sha256:ba2f14ba2670b8bb69059edb4bb555f10a31577f3298ecce78e0a428a6ad09a0`. |

The first aggregate host-gate attempt was rejected because copied run trees did
not include per-run `public/preparation.json`; the corrected `r2` archive keeps
that execution binding. A separate v99 attempt was rejected because a v34 native
manifest carried the wrong APP source seal. Both failures are retained in
`docs/failure-log.md` and demonstrate the intended fail-closed reuse rules.
