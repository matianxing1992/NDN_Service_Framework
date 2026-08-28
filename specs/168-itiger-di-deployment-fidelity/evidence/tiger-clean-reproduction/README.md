# Clean-allocation reproduction

Not admitted. T016 requires an accepted small **and** large correctness profile
before one clean three-node comparison allocation. The large profile is not
accepted: v93 Job 182780 terminated at the model-preparation memory-cgroup OOM
boundary and the sole permitted v94 resource replacement (Job 182782) reached
three CUDA runtime-ready checkpoints but failed its exact deterministic
reference check (`TOKEN_MISMATCH`). No additional large-model allocation will
be submitted under the current campaign.
