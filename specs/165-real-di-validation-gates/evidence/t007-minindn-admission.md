# T007 MiniNDN Admission Evidence

Status: PASS.

`Experiments/ndnsf_validation/evidence.py` validates generation count,
warmup/measurement phases, token and timing completeness, reference-token
agreement, immutable identities, and wire request lineage. The default runner
admits Gate B only after the real process exits successfully and this evidence
validator passes.

The closure Gate B record is `PASS` at `REAL_MININDN_MODEL`; it contains eight
rows, six measured rows, exactly 64 distributed token requests, no missing
metrics, and no request-ID mismatch.
