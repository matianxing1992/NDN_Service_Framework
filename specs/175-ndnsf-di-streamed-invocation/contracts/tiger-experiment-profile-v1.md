# Spec175 Tiger Profile: Transferred

**Status**: Historical interface and incident provenance only.

Spec175 no longer builds a SIF or runs TigerCluster. The useful operational
constraints from this former profile—local SIF construction, Apptainer 1.5.3,
`--cleanenv`, `/bundle` working directory, isolated identity stores, exact hash
staging, external content-addressed models, child-exit propagation, and bounded
cleanup—are re-specified under
`specs/180-ack-driven-cross-model-qualification/contracts/`.

The existing files under `packaging/ndnsf-di-container/jobs/spec175/` and all
recorded jobs remain historical inputs. Spec180 must create its own checked-in
profile, candidate identity, submit interface, and evidence chain rather than
renaming or silently promoting a Spec175 candidate.

