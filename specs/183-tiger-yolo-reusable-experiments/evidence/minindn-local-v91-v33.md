# Spec183 exact local composition: APP v33

Updated 2026-09-10. This is a real local CPU run of the same layered
base+application composition staged for Tiger. It is a component/runtime gate,
not Tiger GPU qualification.

| Field | Value |
| --- | --- |
| Run | `minindn-local-20260910-v91-v33` |
| Case | `local-cpu` |
| Qualification | `NORMAL_EXPERIMENT_PASS` |
| Candidate | `sha256:0c02effbe5f47a55b935724507d60aec718cb92cc35dfb4ec66aa20606f8df5c` |
| Graph | `sha256:d8b40347e4cb60e7a0f74b3503d04816ba897a4e8f8733e9d59a38a8c9a65ed1` |
| Collection input | `sha256:ccfc7d4deeca5aaa404109ea2cb56294436ce8c7fb87937a737030d8a31da715` |
| Requests | 2 (warmup + measured) |
| Numerical result | `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.0005340576171875` |
| Runtime | base SIF `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`, APP v33 manifest `sha256:2df82daa7f5684ebda690fa325e054ca3d992da1d36a6b2fcbda54af343f2b42` |

The maintained operator sequence was `submit.py prepare` followed by
`submit.py local`; provisioning the same run first is rejected as an already
started run. The receipt is retained under
`Experiments/TigerCluster/results/minindn-local-20260910-v91-v33/`.
