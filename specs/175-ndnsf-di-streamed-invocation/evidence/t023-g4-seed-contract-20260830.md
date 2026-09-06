# T023 exact-SIF seed-contract correction (2026-08-30)

The first exact-SIF replay attempt for the current candidate was stopped before
its terminal manifest.  The invocation supplied `--seed 1750001` to the
replay driver, and the driver forwarded that value to every M01--M14 case.
That violated the frozen Spec175 case-seed contract: M01--M04 and M10--M14
use workload seed `1750001`, while M05--M09 use fault seed `1750002`.
Consequently, the partial output root
`results/spec175/g4/exact-current-20260830f/` is incomplete diagnostic evidence
and is not a G4 result.

The tracked `replay-exact-sif.py` driver now derives the effective seed from
the registered fault map, records `seed` in each replay entry, and emits the
workload/fault seed contract in the replay manifest.  The quickstart and
iTiger pre-Tiger checklist state the same rule.  Focused validation passes
(`tests/python/test_spec175_sif_preflight.py`: 28 tests).

The exact-SIF M01 smoke preceding this correction passed with four Providers,
the exact candidate SIF, and terminal evidence.  After the driver correction,
the full 42-entry replay must start from a fresh output root; no partial or
single-seed replay entry may be reused.
