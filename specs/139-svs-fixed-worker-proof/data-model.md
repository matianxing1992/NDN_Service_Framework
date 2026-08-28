# Data Model

- **QualificationSummary**: subject, protected digests, CPU map, two cell
  metrics, gates, and admitted flag.
- **CampaignManifest**: fixed rate, exact six cells, hashes, and no-retry rule.
- **FormalReceipt**: ordinal, pair, mode, terminal status, admission, raw hashes,
  and quiescence record.
- **Analysis**: run metrics, three paired contrasts, stage/traffic/host tables,
  and conclusion.

```text
DESIGNED -> QUALIFIED -> SEALED -> SIX_RECEIPTS -> ANALYZED -> FROZEN
```
