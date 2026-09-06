# Spec175 Quickstart: Local Closure Only

1. Run the code-aware convergence procedure from
   `.specify/memory/design-code-convergence.md` and require `audit.md` to report
   `PASS` for the named frozen source subject.
2. Run the complete relevant native and Python suites from T021 without changing
   source, dependencies, configuration, or the evidence parser.
3. Run the two registered CPU/MiniNDN flows from T022: cold streamed generation,
   then same-conversation continuation plus mismatch rejection.
4. Validate all protocol oracles, child exits, cleanup, and source identities.
5. Record `LOCAL_FUNCTIONAL_PASS` or `LOCAL_UNQUALIFIED` and complete
   `handoff-to-spec180.md`.

Stop and return to T020 after any behavior-affecting change. Do not build a SIF,
upload artifacts, submit Slurm, or use TigerCluster from Spec175.
