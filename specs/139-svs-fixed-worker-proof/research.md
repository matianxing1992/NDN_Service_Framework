# Research Decisions

## Fixed 600 pps

Spec 138 measured 800 pps control as valid and pressured, but the worker missed
the offered-load gate by 0.0417 percentage points. The next lower registered
and previously unobserved rate is 600 pps. Linear CPU projection from the
control gives about 12.7% Face production CPU, above the 10% pressure gate.

## Same Binary

Reuse binary SHA `c4f3b296...35ac`; rebuilding would weaken rather than improve
the same-binary contrast.

## Local Necessity

Necessity is limited to meeting the registered Face-responsiveness boundary at
600 pps. Universal necessity is not testable in one campaign.
