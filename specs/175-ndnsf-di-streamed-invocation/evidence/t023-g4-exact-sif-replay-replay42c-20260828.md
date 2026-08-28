# T023 G4 exact-SIF replay (replay42c)

**Date:** 2026-08-28
**Candidate:** `spec175-final-candidate-replay42c`
**Result:** `PASS` for the repaired, auditable 42-entry aggregate; the first
unmodified replay is retained as a failed diagnostic input.

## Frozen subject

- Source revision: `e4d67cb847b539ebea3eb835f068121d54ba7a18`.
- Sealed source archive: the local candidate's sealed `source-current` archive
  (working directory intentionally excluded from Git).
- Source-seal field: `sha256:24f493fb80a60fe6e14f137258b8a738ed14bf0ca6ba73532b99d64b0b651104`.
- Source-seal JSON SHA-256: `sha256:81b3b7e933fc9c66ea8eb2f01d7a6abbfe561cade9fd383b17478e5689b6a26e`.
- Local SIF: candidate `spec175-runtime.sif` (local release artifact; path is
  intentionally excluded from Git).
- SIF SHA-256: `sha256:63539a1adffa4d8500c56d34104d81971aa72a29958723cd35143bd52b98fbd1`.
- SIF size: 3,520,245,760 bytes.
- Apptainer: `/opt/apptainer/1.5.3/bin/apptainer` (1.5.3).
- Definition SHA-256: `sha256:11ce151938d8e144ca64022a3310eb16b3a1d75efae39ff431a529238647b9ea`.
- Build record: local candidate `build-record.json` (path intentionally
  excluded from Git), record digest
  `sha256:a4ab536046b7f738b1e7ee58e61f9294d523240281bee2af8a6ca6e069756f69`.

The build used the container-native runtime boundary. The temporary
definition enabled `BOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES` for the Boost
1.71 Phoenix header incompatibility; this was a build-input workaround only
and was not added to the repository source.

## Gate evidence

| Gate | Evidence | Digest | Status |
|---|---|---|---|
| G0 | `results/spec175/g0/qualification-manifest-replay42c-20260828.json` | `sha256:cedbee73a030e1969dbd380a0d9616435212b4c6b0fc3f0a500faf48495acc63` | PASS |
| G1 | `results/spec175/g1/qualification-manifest-replay42c-20260828.json` | `sha256:c31bcd9717d9ee99ab9d44fbec1cbbd50f50933e86801316a03a8a5a205b386a` | PASS |
| G2 | `results/spec175/g2/qualification-manifest-replay42c-20260828.json` | `sha256:4eb2aaa96b59c60093f487d48dec8850457d2ec851bfef1ff26b04e4e027d5f3` | PASS |
| G3 | `results/spec175/g3/spec175-g3-post-replay42c-20260828.json` | `sha256:ac438ba5efb9fe814b2b772247267e4da121f78e7c73340f240892de85f56edc` | PASS, 42/42 |
| G4 host | `results/spec175/g4/host-substrate-preflight-replay42c-root-20260828.json` | `sha256:22ae5d17491b7dac213b1c1a179ec4c54ed2ff2a0c55c8148f4f721eb582a5eb` | PASS |
| G4 SIF | `results/spec175/g4/sif-runtime-preflight-replay42c-20260828.json` | `sha256:fd3b493459cee0fc570b05835a68521dd352c9aa33b8d7a9da3a6d2d17bd2ef1` | PASS |

The SIF preflight imported the embedded CPython 3.10 extension, verified its
and the native Provider's `ldd` closure, found ONNX Runtime 1.20.0 with CUDA,
TensorRT, and CPU providers, and found no deployed PyTorch/Transformers
runtime. The host preflight verified the host-only MiniNDN/Mininet/Open
vSwitch/NLSR/namespace substrate and Apptainer version.

## Exact-SIF replay and the one replacement

The original host-orchestrated replay is preserved at
`results/spec175/g4/exact-sif-replay-replay42c-20260828.json` (SHA-256
`sha256:9dba244395b95e615a6bef10c55d5caaec75e4510f754513029cbc819138bdf9`).
It contains 42 entries: 41 `PASS` and one `FAIL` (`M14-r1`). The failed
process reached the real SIF runtime but hit a transient startup race before
Selection: V3 reported no feasible Stage1 Provider. Its complete directory
and log remain at `results/spec175/g4/M14-r1`; it is not deleted or hidden.

The same exact SIF, source seal, host gate, fixed seed (`1750001`), topology,
four Providers, disabled admission control, and workload were then run in a
separate process for `M14-r4`. It passed with the real Provider path and
conversation evidence (`results/spec175/g4/M14-r4/spec175-case-result.json`,
SHA-256 `sha256:f39afeae0a06e499f5b6e285073422c9eaa7cf0bb531b45448e25ee3b0062299`).

The repaired aggregate is
`results/spec175/g4/exact-sif-replay-replay42c-repaired-20260828.json` (SHA-256
`sha256:abdf457a8a50f3088c48569b50c0410fc98a4f4934c1280d65f09f0b10b09c69`).
It has 42 `PASS` entries: the 41 original passing entries plus an explicit
`M14-r1-replacement` record referring to M14-r4. This is an auditable
replacement, not a claim that the first 42-process attempt was clean. The
original failure and replacement metadata are both bound in the repaired
manifest.

The repaired aggregate closes the local T023/G4 evidence boundary for the
next gate under the repository's bounded replacement policy. It does not
claim Tiger execution, CUDA Qwen3.6-27B stateful readiness, multi-Provider
performance, or a clean no-retry first pass. T025 remains responsible for the
bounded current-SIF Tiger control and the G5 model/state qualification.
