# Spec186 r38 replay import diagnosis — 2026-09-15

This receipt explains the r38 catalogue-wrapper failure. It is a source-fix
receipt and does not promote r38 to MiniNDN or GPU qualification.

| Field | Value |
| --- | --- |
| failing build/launch | Slurm job `212306` |
| preserved final SIF | `/project/tma1/ndnsf-di/candidates/spec186-r38-runtime/final.sif` |
| preserved SIF SHA256 | `e890b5de2ffffb9e795cd9b2a7cce28b58cef4b7adcb9fe13844bcfa47c168dd` |
| raw-exception probe | Slurm job `212356` |
| diagnostic image | same r38 final SIF, Apptainer 1.5.3 |
| source failure path | `/opt/ndnsf-di/replay/repo/pythonWrapper/ndnsf/__init__.py` |

## Raw exception

The corrected probe registered its dynamically loaded runner module in
`sys.modules` before execution and exposed the original exception:

```text
ImportError: cannot import name '_ndnsf' from partially initialized module 'ndnsf'
(most likely due to a circular import)
(/opt/ndnsf-di/replay/repo/pythonWrapper/ndnsf/__init__.py)
...
RunnerError: CANONICAL_CATALOGUE_VERIFY_FAILED
```

The preceding probe (`212355`) failed in the probe itself because
`importlib.util.module_from_spec` was used without registering the module in
`sys.modules`; it is retained as diagnostic-process evidence only.

## Fix and bounded verification

`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` now checks the active
interpreter's site-packages for `_ndnsf*.so` and `_py_repoclient*.so` before
adding replay source wrapper roots. Its child path keeps
`/opt/venv/lib/python3.10/site-packages` first and omits same-named source
wrappers when the compiled package is installed. The regression test
`test_runtime_pythonpath_does_not_shadow_installed_native_bindings` verifies
the rule. Local checks passed:

```text
tests/python/test_spec180_yolo_minindn.py: 100 passed
Experiments/TigerCluster/tests/test_development_runtime_template.py
Experiments/TigerCluster/tests/test_spec186_candidate.py: 50 passed
```

The existing r38 SIF is immutable historical evidence and does not contain
this source fix. A new source-sealed candidate must be built before rerunning
MiniNDN or claiming any TigerCluster result.
