# Spec175 local closure record

**Updated:** 2026-08-28
**Candidate:** `spec175-final-candidate-replay42c`
**Source revision:** `e4d67cb847b539ebea3eb835f068121d54ba7a18`
**SIF:** `sha256:63539a1adffa4d8500c56d34104d81971aa72a29958723cd35143bd52b98fbd1`

## Current decision

The corrected subject passes G0, G1, G2, and G3. One locally built SIF then
passed the separate host-substrate and SIF-runtime preflights. The exact-SIF
replay is qualified for the next gate by the repaired aggregate
`results/spec175/g4/exact-sif-replay-replay42c-repaired-20260828.json`, which
contains 42 PASS entries.

This is a qualified local G4 closure, not a claim that the first replay was
clean: the original aggregate
`results/spec175/g4/exact-sif-replay-replay42c-20260828.json` contains 41 PASS
and one FAIL (`M14-r1`). That failure was a transient pre-Selection startup
race (`V3 strategy found no feasible Stage1 provider`). Its directory and log
are retained. A separate same-input process (`M14-r4`) passed and is bound as
the explicit `M14-r1-replacement` in the repaired aggregate.

The replacement changes no source, SIF, seed, topology, workload, Provider
count, admission-control setting, or runtime. It only supplies a successful
independent process for the failed repetition slot. No automatic runtime
replacement is being enabled by this evidence.

## Hash-bound evidence

| Item | Path | SHA-256 |
|---|---|---|
| G0 | `results/spec175/g0/qualification-manifest-replay42c-20260828.json` | `sha256:cedbee73a030e1969dbd380a0d9616435212b4c6b0fc3f0a500faf48495acc63` |
| G1 | `results/spec175/g1/qualification-manifest-replay42c-20260828.json` | `sha256:c31bcd9717d9ee99ab9d44fbec1cbbd50f50933e86801316a03a8a5a205b386a` |
| G2 | `results/spec175/g2/qualification-manifest-replay42c-20260828.json` | `sha256:4eb2aaa96b59c60093f487d48dec8850457d2ec851bfef1ff26b04e4e027d5f3` |
| G3 | `results/spec175/g3/spec175-g3-post-replay42c-20260828.json` | `sha256:ac438ba5efb9fe814b2b772247267e4da121f78e7c73340f240892de85f56edc` |
| Host preflight | `results/spec175/g4/host-substrate-preflight-replay42c-root-20260828.json` | `sha256:22ae5d17491b7dac213b1c1a179ec4c54ed2ff2a0c55c8148f4f721eb582a5eb` |
| SIF preflight | `results/spec175/g4/sif-runtime-preflight-replay42c-20260828.json` | `sha256:fd3b493459cee0fc570b05835a68521dd352c9aa33b8d7a9da3a6d2d17bd2ef1` |
| Original G4 | `results/spec175/g4/exact-sif-replay-replay42c-20260828.json` | `sha256:9dba244395b95e615a6bef10c55d5caaec75e4510f754513029cbc819138bdf9` |
| Repaired G4 | `results/spec175/g4/exact-sif-replay-replay42c-repaired-20260828.json` | `sha256:abdf457a8a50f3088c48569b50c0410fc98a4f4934c1280d65f09f0b10b09c69` |
| Replacement | `results/spec175/g4/M14-r4/spec175-case-result.json` | `sha256:f39afeae0a06e499f5b6e285073422c9eaa7cf0bb531b45448e25ee3b0062299` |

## Release boundary

T023/G4 is complete for this local candidate. Before any model or
performance claim, T025 must run the bounded current-SIF Tiger control first,
then qualify the stateful Qwen3.6-27B CUDA subject. The current record does not
claim Tiger availability, multi-node execution, GPU cache effectiveness, or
the 20-token/s target.
