# Spec178 dataset registration evidence

The registered dataset is `generated-blue-suv-multiview-controlled-v1`. The checked-in six-view vehicle fixture is synchronized, grouped as one target/capture window, and frozen into deterministic 1/2/4/6-view prefixes by `prepare_coperception_uav.py`. Each view has a content digest, capture time, and nominal camera pose; the registration records the MIT license, model profile, preprocessing policy, source-manifest digest, and sample-size rationale.

This fixture is an execution and lifecycle qualification subject, not a calibrated flight dataset. `scientificAccuracyClaimAllowed` is therefore `false`; the paired report is intentionally claim-neutral and does not establish vehicle-recognition accuracy or a multi-view benefit. A future quantitative claim requires additional independently grouped, licensed flight data without changing the registered analysis.

The generated registration is `results/uav-mvcnn-onnx-fixture/registration.json` (SHA-256 `e8a19fd837995e799aace88c846f0178a5273533ee4dbb39cd7ad343b7124556`).
