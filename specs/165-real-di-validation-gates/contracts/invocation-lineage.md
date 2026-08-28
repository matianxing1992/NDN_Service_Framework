# Contract: Invocation Lineage

## Canonical identity

The User creates `requestId` once. The durable identity is:

```text
(requestId, attempt, planId/selectionDigest, modelIdentityDigest)
```

No provider, stage, retry adapter, or evidence writer may replace `requestId`.

## Admission

Before an event affects state, the receiver verifies:

- authentication and authorization appropriate to the existing NDNSF path;
- request ID;
- attempt;
- plan/selection identity when Selection has occurred;
- model identity;
- provider and role assignment;
- event-specific token, epoch, and sequence rules.

An event that fails any check is appended as `admitted=false` with a stable
rejection reason. It cannot execute a role, complete the request, update the
answer, or renew a deadline.

## Required event coverage

Every measured invocation retains ordered evidence for:

- Request;
- each accepted or rejected ACK;
- Selection;
- admitted and rejected operation-status events;
- intermediate role execution and handoff;
- Response;
- terminal admission decision.

## Negative matrix

Tests must inject wrong request, attempt, plan, model, provider/role, duplicate
sequence, reordered sequence, and post-terminal events. Every case must remain
visible and have no state-changing effect.
