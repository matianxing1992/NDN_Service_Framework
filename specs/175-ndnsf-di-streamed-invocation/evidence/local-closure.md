# Spec175 local closure record

**Updated:** 2026-08-29
**Candidate:** `spec175-runtime-fe285147`
**Source revision:** `15f823a4bb5a158be86df81074aae48d2a0244d7`
**SIF:** `sha256:6d905dbb44a65b02dcd091fb1c9a89b81ce7bb86551acf16db2a620c8602e7d7`

## Current decision

The current source passes G0, G1, G2, and G3. One locally built SIF then
passed the separate exact-SIF runtime and host-substrate preflights. The
hash-bound exact-SIF replay
`results/spec175/g4/exact-sif-15f823a4/replay-manifest.json` contains 42/42
PASS entries: M01--M14, three independent processes per case, with the
registered M05--M09 negative or fault-injection oracles retained as PASS
results rather than treated as successful service responses. M11--M14 use the
real Provider conversation path.

The current G4 aggregate is a fresh replay with no replacement entry and no
overlay. The earlier replay42c aggregate and its explicit M14 replacement are
historical diagnostic evidence only; they are not mixed into this candidate.

The bounded M01 exact-SIF smoke also passed with four Providers and eight
token events followed by a final Response. Its reported `distributed_ms`
(8.0--8.9 s in the recorded runs) is an end-to-end tiny-ONNX CPU smoke metric,
not a GPU prefill/decode benchmark; the run does not contain separate prefill
or decode timing fields and cannot establish 27B CUDA performance.

## Current hash-bound evidence

| Item | Path | SHA-256 |
|---|---|---|
| Source seal | `results/spec175/g0/source-seal-15f823a4.json` | `sha256:aedf3cf8faa870d07e41bf5a1865928f579305fb924430ee58781a3e16abb7a6` |
| G0 qualification | `results/spec175/g0/qualification-manifest-15f823a4.json` | `sha256:ad3f1dedccd7a09adddd389dbae16b6b9c8b117f1a914afead5cd9f479608201` |
| G3 host matrix | `results/spec175/g3/host-minindn-manifest-fe285147.json` | `sha256:62caf8067d7049093a1894dd520774443b5cc0d76444aef526ee2957f88b659f` |
| SIF runtime preflight | `results/spec175/g4/sif-runtime-preflight-15f823a4.json` | `sha256:b3c6f659b46a26c6d1bc53b0405c842e58c9b9a604d57ee4cad7de018b8f2cc9` |
| Host-substrate preflight | `results/spec175/g4/host-substrate-preflight-15f823a4.json` | `sha256:7071aba20316fab603c0f562f7d58adc987a10363a8636ad6b04f08641d3ac1e` |
| Exact-SIF replay | `results/spec175/g4/exact-sif-15f823a4/replay-manifest.json` | `sha256:4845c502cbf1fbac2dc91cb44d3468110b30e067817766fadcaac71dee9d7359` |

## Release boundary

T023/G4 and T025 are complete for the current candidate. T025 is recorded in
`evidence/t025-tiger-control-206901.md` and
`evidence/tiger-stage-readiness.md`: the current SIF passed the four-Provider
CPU control and the pinned three-stage Qwen3.6-27B CUDA state/cache-readiness
gate on Tiger jobs `206901` and `206907`. T026--T028 and T034 remain open: no
current record here claims multi-Provider generation, conversation residency,
or the 20-token/s performance target. The next valid step is the current-SIF
three-Provider functional gate, followed serially by conversation-residency
and performance gates.

## Historical diagnostic evidence

The 2026-08-28 replay42c records remain retained for audit and explain the
earlier M14 startup race, but their source revision and SIF hash differ from
the current candidate. They must not be cited as current qualification.
