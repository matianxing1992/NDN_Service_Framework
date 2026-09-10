# Spec183 exact-SIF MiniNDN matrix: APP v33

Updated 2026-09-10. These runs execute the fixed APP v33 against the unchanged
base SIF and retain the normal and negative MiniNDN lifecycle records.

| Run | Matrix | Result | Scope |
| --- | --- | --- | --- |
| `minindn-local-20260910-v86-v33` | Y-B | PASS; `shape=[1,50,6]`, `matched=true`, `maxAbsError=0.0005340576171875` | Normal graph and numerical oracle |
| `minindn-local-20260910-v88-v33` | Y-N | PASS; all eight registered subcases, including expired/forged/wrong-recipient authorization, missing dependency and malformed input | Authorization, dependency and cleanup matrix |

The base SIF is `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5` and the APP manifest is
`sha256:2df82daa7f5684ebda690fa325e054ca3d992da1d36a6b2fcbda54af343f2b42`.
These are MiniNDN/exact-composition gates only; they do not establish a real
Tiger allocation or GPU qualification.
