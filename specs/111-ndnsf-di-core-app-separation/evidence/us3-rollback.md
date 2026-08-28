# US3 Rollback

Verdict: **PASS**

Legacy deployment records are translated by the thin adapter into the same
immutable Core `AssignmentContext`; they do not recreate ambient environment
state. Mixed legacy/new and malformed-context tests passed. The rollback façade
preserves Core validation, attempt epoch, original deadline and exclusion
lineage without reintroducing the removed preference environment variable.
