# T003 Progress Deadline Evidence

Status: PASS.

The deterministic admission/deadline state machine is implemented in
`Experiments/ndnsf_validation/deadlines.py`. Tests use an injected clock and
cover valid advance, duplicate, reorder, forgery, wrong binding, invalid total,
idle stall, immutable hard cap, equal-boundary precedence, first-terminal-wins,
and post-terminal rejection.

The final Spec 165 suite passed all deadline cases without wall-clock sleeps.
`STALLED` and `HARD_TIMEOUT` remain distinct terminal reasons.
