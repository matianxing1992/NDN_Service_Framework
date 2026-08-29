# T023 G4 exact-SIF replay (current source)

Date: 2026-08-29  
Branch: `Experimental`  
Source revision: `15f823a4bb5a158be86df81074aae48d2a0244d7`  
SIF: local candidate `spec175-runtime-fe285147.sif` (path and hash are bound by the build record)  
SIF SHA-256: `sha256:6d905dbb44a65b02dcd091fb1c9a89b81ce7bb86551acf16db2a620c8602e7d7`  
Apptainer: `/opt/apptainer/1.5.3/bin/apptainer` (1.5.3)

## Qualification inputs

The G3 subject was regenerated after fixing the fault-seed validator. Ordinary
cases use workload seed `1750001`; M05--M09 use the registered fault seed
`1750002`. The resulting frozen host matrix contains 14 cases × 3 fresh
processes = 42/42 PASS, four Providers, tiny-ONNX, and admission control
disabled. No source path was changed after this seal while building the SIF.

| Artifact | Path | SHA-256 |
|---|---|---|
| G0 source seal | `results/spec175/g0/source-seal-15f823a4.json` | `sha256:aedf3cf8faa870d07e41bf5a1865928f579305fb924430ee58781a3e16abb7a6` |
| G0 qualification | `results/spec175/g0/qualification-manifest-15f823a4.json` | `sha256:ad3f1dedccd7a09adddd389dbae16b6b9c8b117f1a914afead5cd9f479608201` |
| G3 host matrix | `results/spec175/g3/host-minindn-manifest-fe285147.json` | `sha256:62caf8067d7049093a1894dd520774443b5cc0d76444aef526ee2957f88b659f` |
| SIF build record | local candidate build record | `sha256:2d5119e8c8946d3f6961f27bcda7dce8dc7860d04ef61de4bfa5fdd2edca050e` |
| SIF runtime preflight | `results/spec175/g4/sif-runtime-preflight-15f823a4.json` | `sha256:b3c6f659b46a26c6d1bc53b0405c842e58c9b9a604d57ee4cad7de018b8f2cc9` |
| Host substrate preflight | `results/spec175/g4/host-substrate-preflight-15f823a4.json` | `sha256:7071aba20316fab603c0f562f7d58adc987a10363a8636ad6b04f08641d3ac1e` |
| G4 replay manifest | `results/spec175/g4/exact-sif-15f823a4/replay-manifest.json` | `sha256:4845c502cbf1fbac2dc91cb44d3468110b30e067817766fadcaac71dee9d7359` |

## Results

- G0: PASS, zero blockers.
- G3 host/CPU matrix: 42/42 PASS.
- SIF runtime preflight: PASS. The image reports Python 3.10.18, ONNX
  Runtime 1.20.0 with `TensorrtExecutionProvider`, `CUDAExecutionProvider`,
  and `CPUExecutionProvider`, closed `ldd` for the Provider and CPython
  extension, and no deployed PyTorch/Transformers modules.
- Host-substrate preflight: PASS under root; MiniNDN, Mininet, OVS, NLSR,
  `mnexec`, and Apptainer 1.5.3 were found on the host layer.
- Exact-SIF replay: PASS, all 42 entries returned code 0 and all 14 cases
  have exactly three entries. M05--M09 retain their expected terminal-error
  contract; M11--M14 report real Provider conversation paths.
- A bounded exact-SIF M01 smoke completed before replay with four Providers,
  eight streamed token events, and a final Response. Its measured
  `distributed_ms=8627.33` is an end-to-end tiny-ONNX CPU smoke metric, not a
  GPU prefill/decode measurement.

This closes the local T023/G4 boundary and authorizes the next serial step,
T025/T026 Tiger execution. It does not claim CUDA multi-Provider execution,
conversation residency, or performance; those remain T026, T034, and T027.
