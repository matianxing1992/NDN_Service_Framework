# Spec 170 evidence identity

Gate evidence is bound to a single candidate and run by
`tools/ndnsf-di/spec170_evidence.py`.  The candidate digest covers source,
OCI, SIF, dependency lock, model, canonical artifacts, prompt corpus,
security policy, route, schedule, and freeze timestamp.  Complete and negative
rows are retained separately.  After `freeze()` any mutation is rejected as
`INVALID_CANDIDATE`; no TigerCluster result may be merged into a different
candidate identity.
