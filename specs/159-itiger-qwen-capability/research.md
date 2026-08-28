# Research Decisions

## Candidate authority

**Decision**: Treat Spec 158 as a development capability candidate.  
**Rationale**: Its local closure and reuse gates pass, but the source was dirty
and no immutable registry digest or live GPU evidence exists.  
**Alternative rejected**: Calling it a formal release before GPU validation.

## Initial model and placement

**Decision**: Use the already staged Qwen2.5-0.5B revision and prefer a bounded
single-GPU allocation on itiger07.  
**Rationale**: It is the smallest frozen model and itiger07 has a recorded
successful account/Apptainer preflight.  
**Alternative rejected**: Starting with multi-GPU or a larger model.

## Ordered gates

**Decision**: SIF, GPU runtime, standalone model, then real NDNSF-DI.  
**Rationale**: Each gate isolates a distinct failure boundary and prevents a
standalone result from being mislabeled as framework validation.
