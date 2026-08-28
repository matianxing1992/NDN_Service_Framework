# T020 post-audit G0-G2 closure (2026-08-27)

## Subject

- Source revision: `451654fcad6f10034851cac000448d454ca8a582`
- System toolchain: `/usr/bin/g++-9` and `/usr/bin/ld.bfd`
- Boost: 1.71 only
- NDN-SVS: `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`
- ONNX Runtime: `/opt/onnxruntime/lib/libonnxruntime.so.1`
- Source seal: `results/spec175/g0/source-seal-post-audit-r3-20260827.json`
- Source-seal SHA-256: `sha256:9f053b16cde722e80271367e64662d8dd9c34d5ab1e659647a894e8b925e9af7`
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
| G0 | PASS; 0 blockers; 73 FR and 16 SC fully traced | `results/spec175/g0/spec175-contract-post-audit-r3-20260827.json` |
| G1 | PASS; native unit 600/600; Python 218 passed, 0 failed/skipped; extension import/ldd ready | `results/spec175/g1/spec175-python-post-audit-r3-20260827.json` |
| G2 | PASS; 38/38 process results across registered I01-I20; no missing case | `results/spec175/g2/spec175-integration-post-audit-r3-20260827.json` |

G1 and G2 both bind the exact source-seal SHA-256 above. Their manifest
SHA-256 values are:

- G1: `sha256:1f84fa11176850e7d033adedd1ae80dc147cf098e8467d19548391bcb22b6294`
- G2: `sha256:3f8ff03e7bd62863979b1cd0466fdc2177cdf8531ebcfbed91510c75d97f8faa`

## Decision

T020 is closed for this source revision. T022 remains open because the existing
42/42 G3 manifest binds an earlier source seal. No SIF build, upload, Slurm
allocation, or Tiger execution is authorized until the unchanged 42-case G3
matrix passes against this exact source seal.
