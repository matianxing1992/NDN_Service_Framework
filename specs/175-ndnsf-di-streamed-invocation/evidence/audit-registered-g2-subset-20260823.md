# Spec175 registered G2 subset audit — 2026-08-23

Status: `HISTORICAL_SUBSET_PASS`; this snapshot predates controlled I12
registration and is not complete G2 because the exact live-Provider I13
boundary is only a lower-bound oracle, and healthy cases have not yet completed the required three fresh
process repetitions under T020.

## Subject and command

- `build/integration-tests` SHA-256:
  `d5ffc604a6d3abda545b6a4bf7a9e55663b1516930bde8a2bcae157333057ea5`
- Seed: `1750001`

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I11,I14-I15 \
  --healthy-repeats 1 \
  --seed 1750001 \
  --output /tmp/spec175-g2-registered-subset.json
```

## Result

All 13 registered cases returned zero in independent child processes:

```text
I01 I02 I03 I04 I05 I06 I07 I08 I09 I10 I11 I14 I15
```

The manifest status was `PASS` for the explicitly selected registered subset;
its SHA-256 was
`8910de74efbfd35f27a6afb35f4c7534e136bb4ede5ee95254a827b0029d6e51`.
The gate's complete default I01-I15 request remains blocked until the I13
live-boundary proof is added and the complete matrix is rerun.

This run includes the attempt-epoch regression discovered by the audit: I02
initially failed because the native coordinator received attempt 0. After the
coordinator consumed the signed stream attempt epoch, I02 again completed two
Provider coordinators, eight token events, and one final Response. I07 also
asserts that each permanent-gap error carries a nonempty absolute Provider
identity from the validated stream binding, which is required before an opt-in
replacement can exclude a Provider.
