# Spec 111 CodeGraph Baseline

## Commands

```bash
codegraph status .
codegraph explore "runtime_v1 APPClient APPDeployment deployment.py root exports native distributed inference callers packaging experiments"
codegraph query APPClient --path . --limit 10
codegraph query APPDeployment --path . --limit 10
codegraph query DistributedLeaseTransaction --path . --limit 10
codegraph query NativeProviderHandler --path . --limit 10
```

## Index identity

- Files: 2,341
- Nodes: 50,736
- Edges: 167,899
- Backend: built-in SQLite with WAL
- Status: up to date

## Verified ownership baseline

| Surface | Current source fact | Initial migration consequence |
| --- | --- | --- |
| `APPClient` | `app.py:241`; package root imports it from `.app` | APP façade and engine composition move behind `app_sdk`, with root compatibility delegation |
| `APPDeployment` | `app.py:861`; exposes definition accessors, not the proposed lifecycle | Implement lifecycle owner in `app_sdk/deployment.py`; do not infer it from configuration loading |
| `DistributedLeaseTransaction` | `deployment.py:623`; existing requester-side prepare/commit/cleanup mechanism | Reuse and extend receipt evidence; do not create a second lease authority |
| `NativeProviderHandler` | `cpp/ndnsf-di/NativeProviderHandler.*`; used by native provider examples | Keep native mechanism stable until characterization; later split model adapters without changing symbols |
| package root | `__init__.py` imports APP, Core-like, planner, ONNX/Qwen/llama, runtime and ops surfaces together | Compatibility manifest must cover root exports before owner-package isolation |
| `runtime_v1.py` | contracts, scoring, cache/state, scheduler, simulations, evidence and CLI coexist | Inventory every decision point before moving defaults or CLI/simulation ownership |

CodeGraph reported two direct indexed callers for `APPClient` through the package
root and the LLM pipeline example, and an external example importing
`APPDeployment`. Text/AST inventory expands this to all tracked Python,
native, example, experiment and deployment-adapter surfaces.

## Evidence boundary

This is source/call-graph evidence only. It proves neither implementation nor
runtime behavior. No host NFD, MiniNDN, container, Slurm or iTiger command was
run to produce it.
