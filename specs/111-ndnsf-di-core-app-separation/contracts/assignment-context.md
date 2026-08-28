# Contract: Assignment Context

`AssignmentContext` is immutable and scoped to one request attempt.

Required fields:

- request/template/plan identity;
- selected model-variant/exact-model, objective and engine-snapshot lineage;
- placement decision, policy and candidate snapshot identity;
- role-to-provider assignments;
- validated lease bindings;
- deadline, attempt and attempt epoch;
- candidate/release/evidence identity.
- committed execution-intent identity and progress/output/checkpoint epoch.

Rules:

1. It is passed explicitly from assignment acceptance into collaboration and
   execution calls.
2. It is never reconstructed from mutable process-global environment state.
3. A legacy deployment record may be translated once into a context, but the
   translator cannot install process state or create authority.
4. Replanning creates a new immutable context and preserves the original
   deadline and exclusion lineage.
5. Every output/evidence record identifies the context/attempt that produced it.
6. Concurrent contexts cannot share mutable assignment maps.
7. A context becomes executable only after its full variant/plan/assignment/
   target intent commits; abort releases reservations and invalidates it.

Forbidden behavior:

- writes to `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE`;
- implicit inheritance from another request;
- policy mutation after decision digest creation;
- accepting stale attempt output under a newer context.
- resuming from a non-advertised checkpoint or publishing duplicate output epoch.
