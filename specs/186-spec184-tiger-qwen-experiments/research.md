# Spec186 Research Notes

## Repository Findings

- `575b43cc93bbed29932303caf3d09974f1585af7` is the requested Spec184 preparation
  baseline; the current remote `Experimental` tip has moved to Spec185 and is not a safe
  base for this work.
- Existing TigerCluster infrastructure contains reusable transport, identity, cleanup,
  Apptainer and Slurm primitives, but the Spec184 experiment must use a new candidate and
  evidence namespace rather than importing Spec183 closure claims.
- `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` is a native C++-first MiniNDN runner.
  Python owns topology and child lifecycle; model execution and DI dataflow remain native.
- The local host can reasonably run Qwen3-0.6B CPU smoke/functional work, but that does
  not close the Spec184 Qwen3.6-27B external row.

## Decision Summary

1. Use file-level migration of stable Tiger primitives; do not merge a whole later branch.
2. Keep base runtime SIF and changing application bundle separate and content-addressed.
3. Treat MiniNDN as a prerequisite evidence plane, not as TigerCluster qualification.
4. Classify Qwen3-0.6B by actual model format/backend; stop on ONNX/GGUF mismatch.
5. Require CodeGraph design-to-code convergence before complete validation.

## Open Inputs

- Exact availability, format and digest of Qwen3-0.6B weights and tokenizer.
- Tiger partition/account, healthy node pair, GPU type, Apptainer version and project-storage root.
- Whether the final Spec184 external Qwen3.6-27B artifact will ever be supplied.
