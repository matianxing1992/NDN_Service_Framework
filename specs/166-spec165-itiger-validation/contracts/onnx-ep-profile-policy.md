# ONNX Execution-Provider Profile Policy

## Scope

This contract governs the frozen Spec 166 Qwen3-0.6B ONNX stages. It is not a
general claim that every use of an operator listed below is control-only in
arbitrary models.

## Required evidence

Each stage must enable ONNX Runtime profiling for the complete standalone
campaign and retain the profile in durable evidence. Acceptance requires:

1. at least one profiled `Node` event;
2. `CUDAExecutionProvider` is the first configured provider;
3. CUDA events cover matrix multiplication (`MatMul` or `FusedMatMul`),
   softmax (`Softmax`), and normalization (`ReduceMean` or
   `SimplifiedLayerNormalization`);
4. no CPU event exists for any operator in those semantic groups;
5. every CPU event has a non-empty operator and node name;
6. every CPU operator belongs to the frozen allowlist;
7. every CPU event records all input/output tensor types and shapes, every
   tensor is `int64` or `bool`, and every tensor contains at most eight metadata
   elements.

## Frozen CPU allowlist

Only these structural operators may execute through `CPUExecutionProvider`:

```text
ConstantOfShape
Add
Concat
Div
Equal
Expand
Gather
Identity
Mul
Range
Reshape
Shape
Split
Squeeze
Unsqueeze
Where
```

The operator name alone is insufficient. The type/shape rule is mandatory: an
allowlisted `Add`, `Mul`, `Gather`, or other operator fails if it observes a
floating tensor, a tensor larger than the frozen metadata bound, or incomplete
type/shape evidence. This permits the graph's dynamic-shape arithmetic without
permitting model-value computation on CPU.

The semantic groups tolerate ONNX Runtime graph optimization. In particular,
Qwen RMS normalization can appear as CUDA `SimplifiedLayerNormalization`
instead of the exporter-level `ReduceMean`; requiring only the pre-optimization
operator name would reject valid CUDA execution.

## Fail-closed behavior

An empty profile, missing provider/operator/name/type/shape, an unknown CPU
operator, a non-metadata CPU tensor, a core CPU event, or missing required CUDA
semantic-group evidence makes the stage and standalone job fail. The result
must retain provider/operator counts, CPU tensor evidence, unique CPU node
names grouped by operator, and all policy violations. A failed profile may
inform a later contract revision, but it must not be relabeled as an accepted
CUDA run.

This v2 rule is grounded in preserved job `181084`: all CPU events across all
three stages used only `int64`/`bool` tensors of at most five elements, while
CUDA executed `MatMul`, `Softmax`, and `SimplifiedLayerNormalization`. The job
remains a failed v1 identity; the evidence justifies v2 but is not retroactively
promoted.
