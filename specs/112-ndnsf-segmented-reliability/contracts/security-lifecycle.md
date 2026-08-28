# Contract: Python Targeted Security And OpenABE Lifetime

## Python Targeted Invariants

- One Python service registration serves both normal and Targeted invocation.
- Provider and User production bindings keep token use enabled.
- Controller permissions, Provider permission, NAC-ABE attribute routing,
  one-time UserToken/ProviderToken validation, and replay rejection remain
  mandatory.
- Missing, mismatched, consumed, or replayed tokens do not invoke the application
  handler.
- Spec 112 exposes no public tokens-off switch.

If the real current binding passes all positive and negative tests before an
edit, email defect 3 is recorded as already corrected and the binding remains
unchanged.

## OpenABE Lifetime Invariants

- Lifecycle tests must initialize and use NAC-ABE; an unused-process exit is not
  evidence for email defect 5.
- OpenABE initialization/use remains on its process-wide owning execution thread.
- Application/static destructors do not invoke unsafe global OpenABE/RELIC
  shutdown ordering.
- Controller, Provider, and User normal/controlled exits record exit code,
  signal, timeout, and sanitizer evidence when available.
- If the current process-lifetime executor and empty destructor pass 100
  initialized lifecycles, no speculative teardown change is made.

## Acceptance

- Valid normal and Targeted Python calls complete once with tokens enabled.
- Every invalid-token case fails closed.
- 100 initialized cycles, each containing Controller, Provider, and User (300
  role exits total), contain no SIGSEGV or SIGABRT.
