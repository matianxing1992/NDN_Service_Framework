# T023 exact-SIF smoke after experiment-control contract

**Date:** 2026-09-02  
**Subject:** post-contract source seal and locally built exact SIF  
**Status:** bounded smoke PASS; full G4 matrix still pending

The candidate was built from the post-correction source archive and the
fixed-seed G3 manifest, using Apptainer 1.5.3 inside the container-native
build boundary.

| Input | Evidence |
|---|---|
| Source revision | `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` |
| Source archive seal | `sha256:26d562630685cc94f15b07e7503f34236cb5ff38b28b2a232a1b264faea81571` |
| SIF | `.local-tmp/spec175-final-candidate-post-contract-20260902/spec175-runtime.sif` |
| SIF SHA-256 | `sha256:6cbac977d085a220fc40c0470a54e5fa4874a2386b870e2ec1db6039202a7ceb` |
| G3 host manifest | `results/spec175/g3/qualification-manifest-post-profile-20260901T221454Z.json` (`42/42 PASS`) |

Both split preflights passed. The in-SIF preflight verified Python 3.10,
CPython extension import, complete `ldd` closure, NDNSF/NDN-SVS/NFD commands,
ONNX Runtime 1.20.0 with CUDA and CPU providers, and no deployed
PyTorch/Transformers modules. The root host-substrate preflight verified the
same G3 manifest, MiniNDN/Mininet/OVS/NLSR inputs, replay-driver contract, and
Apptainer 1.5.3.

The exact-SIF M01 smoke used only the frozen wrapper values: four Providers,
the fixed host topology, disabled admission control, seed `1750001`, ACK
timeout 1500 ms, request timeout 60000 ms, and a five-second initial-SVS settle
interval. The case result is `PASS`; it contains 293 structured lifecycle
events, six request IDs, four Provider stage publications/fetches, ACK,
provider-specific Selection, Response, and zero surviving owned processes.
The terminal record marks bounded forced teardown signals as intentional
cleanup after the terminal response; this is not a graceful-shutdown claim and
does not substitute for the complete 42-entry G4 replay.

The smoke output is retained under the candidate directory. No Tiger upload or
allocation was performed. T023 remains open until a fresh exact-SIF M01--M14,
three-repetition replay produces one candidate-bound `PASS` manifest.
