# Spec 131 Corrected Baseline Block

- Campaign: `spec131-confirm03-20260722T044823Z`
- Manifest SHA-256: `cc3b697bc6879cb5cbad00f83b01c3f4d0379148145a8084daac501a2c8f9c57`
- Subject: `baseline-sync-serial`
- Base commit: `a9944019f76791773604999f00128057b9534ace`
- Formal ordinals: 1-5
- Receipts: 5/5, each `attempt=1`, each `COMPLETE`
- Process exits: 5/5 publisher `0`; 5/5 subscriber `0`
- Treatment cells started before gate: 0

| Rate | Scheduled | Attempted | API-completed | Delivered | Attempted pps | Delivered/attempted | Sender-limited |
|---:|---:|---:|---:|---:|---:|---:|:--|
| 200 | 12000 | 11998 | 11998 | 11998 | 199.967 | 100.000% | no |
| 400 | 24000 | 24000 | 24000 | 24000 | 400.000 | 100.000% | no |
| 600 | 36000 | 35998 | 35998 | 35998 | 599.967 | 100.000% | no |
| 800 | 48000 | 47958 | 47958 | 47892 | 799.300 | 99.862% | no |
| 1000 | 60000 | 59997 | 59997 | 3587 | 999.950 | 5.979% | no |

Raw path:
`results/spec131-svs-pubsub-commit-latency/spec131-confirm03-20260722T044823Z`
