# Spec186 r81 Python-wrapper runtime-path boundary

- Candidate release: `spec186-4c1f2a9e-base1dd96267`.
- Source revision: `4c1f2a9ea6d97ec746d903e21c4337687b42b1e3`.
- Definition SHA: `sha256:7bfbb8086a77c4a0b43872412734d0d5bb00ac41b87f8102f629d55724a372b1`.
- Source seal: `sha256:ead4fa683253ab99144e620b706098fb7bd2d5884718abfc1a2046e0f074fb87`.
- Base SIF SHA: `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c`.
- SIF SHA: `sha256:2213ef1172de96478cfdb979da5df79862accd83d301a5f267ce62dafced7b74`.
- Apptainer: local `/usr/local/bin/apptainer` 1.5.3.

The container-native build completed `284/284` with `./waf -j2`; builder
imports, final-stage imports and missing-library checks passed. The exact
read-only SIF probe then found this `_ndnsf.so` RUNPATH:

```text
Library runpath: [/opt/ndnsf-di/current/lib:/src/ndn-svs/build]
```

The Waf fix removed the path from main binaries, but both setuptools wrapper
link steps still injected it. r81 is therefore retained as
`BUILD_PASS_RUNTIME_BOUNDARY_FAIL` and is not eligible for MiniNDN, upload or
Tiger execution.

The next candidate removes the wrapper `-rpath` flags while retaining the exact
SVS build-tree object for link-time ABI selection; runtime resolution remains
under the staged `/opt/ndnsf-di/current/lib` directory.
