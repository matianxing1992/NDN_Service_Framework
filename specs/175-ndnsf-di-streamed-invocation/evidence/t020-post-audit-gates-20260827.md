# T020 post-audit G0-G2 closure (2026-08-27)

## Subject

- Source revision: `fe0b09fcc5230dd7feb3d35b1d140b40f434da14`
- System toolchain: `/usr/bin/g++-9` and `/usr/bin/ld.bfd`
- Boost: 1.71 only
- NDN-SVS: `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`
- ONNX Runtime: `/opt/onnxruntime/lib/libonnxruntime.so.1`
- Source seal: `results/spec175/g0/source-seal-post-audit-r4-20260827.json`
- Source-seal SHA-256: `sha256:eda21b2b8b05e2961503c8bd07fecafa1d9af0cdd1e72d6d92778c905535a771`
- In-scope dirty files: 0

Feature documents are bound by the G0 manifest's per-document digests rather
than by the binary source seal. This lets task/evidence updates record a gate
result without pretending that the compiled source changed.

## Build and loader closure

`./waf configure` passed with the explicit system toolchain, Boost 1.71, and
the explicit NDN-SVS source/build pair. A fresh
`./waf build --targets=unit-tests,integration-tests -j2` completed
successfully. `ldd` reported zero unresolved libraries for both binaries and
resolved NDN-SVS, Boost, and ONNX Runtime from the paths above.

- `build/unit-tests`: `sha256:79bbfb0aa43e949ed35eed55661052d002561b5e48ece58eeeaadfe9ceeadac5`
- `build/integration-tests`: `sha256:33838e8972ab7a2f790194bd78576abd9c223238ac6013664edc61aa28499180`

## Gate results

| Gate | Result | Durable local manifest |
|---|---|---|
| G0 | PASS; 0 blockers; 73 FR and 16 SC fully traced | `results/spec175/g0/spec175-contract-post-audit-final-20260827.json` |
| G1 | PASS; native unit 600/600; Python 219 passed, 0 failed/skipped; extension import/ldd ready | `results/spec175/g1/spec175-python-post-audit-r4-20260827.json` |
| G2 | PASS; 38/38 process results across registered I01-I20; no missing case | `results/spec175/g2/spec175-integration-post-audit-r4-20260827.json` |

G1 and G2 both bind the exact source-seal SHA-256 above. Their manifest
SHA-256 values are:

- G1: `sha256:80edd53f3c75a149db217e6c0cec77b88af6005d1740eb67c1490a00bb94b77f`
- G2: `sha256:ddaa7cc43d2a423b0ba2c14b335b86e17d3442838beaa133c78021f9a05f0d8b`

## Decision

T020 is closed for this source revision. T022 remains open because the existing
42/42 G3 manifest binds an earlier source seal. No SIF build, upload, Slurm
allocation, or Tiger execution is authorized until the unchanged 42-case G3
matrix passes against this exact source seal.
