# Spec175 current-tree gate evidence

**Command**

```text
python3 scripts/spec175_contract_gate.py --project-root . \
  --feature-dir specs/175-ndnsf-di-streamed-invocation
```

**Observed result**

```text
status: BLOCKED
blocker: DIRTY_INPUT_TREE
detail: 295 dirty in-scope paths; promotion evidence requires a sealed source subject
```

**Interpretation**

The result is expected after Spec180 implementation and qualification files
were added to the shared working tree. It blocks relabeling the current tree
as a new Spec175 candidate; it does not invalidate the frozen T022
`LOCAL_FUNCTIONAL_PASS` source seal. Spec180 owns the current source delta and
must produce its own convergence and qualification evidence.
