# T037 ordinary-V3 boundary repair evidence — 2026-08-31

## Subject

The Spec175 ordinary streamed-invocation boundary in the current working
tree. This is focused repair evidence only; it is not a G0--G3 qualification
manifest and does not authorize a MiniNDN, SIF, or Tiger run.

## Required behavior

The public streamed and automatic-planning paths accept only ordinary
`DI_PLACEMENT_V3` plans: rank zero, tensor degree one, complete role coverage,
one Provider per complete role, and one role per Provider. V2, hybrid, and
TensorGroup/rank-role shapes remain available only to their separate
compatibility APIs and cannot reach Spec175 Selection.

## Focused verification

```text
python3 -m pytest -q tests/python/test_spec175_v3_boundary.py
17 passed in 4.21s
```

The test includes a fresh Python process for each rejected shape, with the
same source package and `py_repoclient` import closure used by the repository
tests. It covers V2 strategy, hybrid plan, tensor degree two, missing role,
duplicate Provider ownership, `TENSOR_RANK`, and non-zero rank. The in-process
tests cover positive one-, two-, and four-role `PreSplitFirst` proposals and
the public `request_streaming`/generic `request` pre-publication gates.

## Result

`T037` ordinary-V3 boundary: **PASS for its focused repair gate**. The separate
post-repair design-to-code audit is still required before formal validation;
T038--T042 and reopened T031 remain controlling Spec175 work.
