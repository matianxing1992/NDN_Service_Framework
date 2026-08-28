# Contract: Targeted timeout_ms

## Deadline

The Targeted deadline starts when the public API accepts and records the request.
Admission, token bootstrap/refill, publication, and response wait consume the
same absolute deadline. No earlier phase can postpone timer creation.

## Terminal Outcomes

For Spec 112, the existing user-visible terminal callbacks are:

- `response`: one validated response invokes the response callback;
- `timeout`: expiration or an otherwise non-completing Targeted request invokes
  the timeout callback.

Exactly one callback wins. Publication exceptions do not remove the deadline or
leave the request pending forever. Late/duplicate responses and late timers
cannot invoke a second callback. Terminal cleanup removes pending request, timer,
and per-request token state.

## Required Tests

- Provider absent before Targeted bootstrap;
- Provider degraded or stopped after Targeted state exists;
- publication exception/non-completion;
- response-versus-timeout race;
- late response after timeout;
- real Python asynchronous callback and bounded synchronous adapter behavior.

Each degraded/absent-Provider case must invoke one timeout callback no later than
`timeout_ms + 500 ms`.

## Scope Boundary

Spec 112 adds no cancellation outcome, explicit remote-failure callback, second
status protocol, or new wire message. Those are not required by email defect 4.
