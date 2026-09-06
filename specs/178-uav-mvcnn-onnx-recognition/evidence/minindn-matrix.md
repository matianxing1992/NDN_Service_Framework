# Spec178 MiniNDN matrix evidence

Command: `sudo -n python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py --execute --all-scenarios --provider /provider/cpu --model-mode real --output /tmp/spec178-minindn-matrix-v3`

Reference runtime: Python 3.8.10, ONNX Runtime 1.19.2, Torch 2.4.1+cpu. The registered artifact was `mvcnn_vehicle_cpu.onnx` (`sha256:14ec256fbc84e1c6d9d0cf593ca47ce151c641c5b3280535ca4dcbedd1a4c317`), executed with `CPUExecutionProvider` only.

| Scenario | Terminal status | Gate | Evidence |
|---|---|---:|---|
| nominal | completed | pass | 6 verified views, selected `/provider/cpu`, MVCNN fusion/inference, one result owner |
| provider-selection | completed | pass | selected-provider path; non-selected Provider published no terminal output |
| unavailable-view | rejected | pass | view retrieval failure is reported before model completion |
| late-view | rejected | pass | deadline/retrieval failure is distinct from model failure |
| publication-failure | rejected | pass | annotation publication failure is terminal and explicit |
| model-missing | rejected | pass | registered artifact absence reaches `inference-failed` without fallback |
| model-digest-failure | rejected | pass | digest mismatch reaches `inference-failed` without fallback |

The retained matrix is `results/uav-mvcnn-onnx-fixture/minindn-matrix.json` with SHA-256 `19e074e2849659b094d3a120e2e19605d1ded66749e66442947d9f2d72f9ccc7`. Full per-run logs/results remain in the immutable campaign output used to derive this summary; no image bytes occur in service payloads.
