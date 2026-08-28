# T022 four-Provider layout checkpoint (2026-08-24)

The production MiniNDN runner now supports a controlled four-stage layout:

```text
Stage0 -> ucla   -> /example/llm-pipeline/provider
Stage1 -> arizona-> /example/llm-pipeline/provider/1
Stage2 -> wustl  -> /example/llm-pipeline/provider/2
Stage3 -> neu    -> /example/llm-pipeline/provider/3
```

Repository identities use the corresponding `/repo[/index]` names.  The
layout is selected immediately after argument parsing, rejects fewer than two
or more than four stages, and is covered by
`tests/python/test_spec175_minindn_layout.py` (16 passing cases together with
the dedicated launcher contract tests).

`Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` now freezes the M01--M10
case names, positive seed requirement, the pinned
`Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` identity, the
four-stage ONNX-native command, and disabled-admission contract.  M02--M10 deliberately return
`BLOCKED_EXPECTED` until a registered live fault controller is connected; a
clean run is not mislabeled as a fault-case result.

This was an input/launch guard only.  It did not claim a real MiniNDN M01--M10
result, G3 promotion, or exact-SIF execution.  A later checkpoint now records
one real healthy M01 run; M02--M10 and the 30/30 matrix remain open. The current G0 manifest remains
blocked solely by the dirty in-scope source tree:

```text
results/spec175/g0/qualification-manifest-current-20260824-layout.json
sha256:fe3f1e87ede95b043da3100028a8644fa007a32bd6b2430c995e142576d45660
status=BLOCKED_EXPECTED
blocker=DIRTY_INPUT_TREE
```

## Superseding status note

The paragraph above is a historical layout-checkpoint snapshot. After the
sealed source-input workflow was added, the current G0 authority became
`results/spec175/g0/qualification-manifest-sealed-20260824.json`, which is
`PASS` with zero blockers. That change does not advance T022 to completion: the
registered M02--M10 fault controller and the complete 30/30 matrix are still
missing.
See `evidence/progress-20260824.md` for the authoritative gate state.
