# Spec 131 Corrected Latest Block

- Campaign: `spec131-confirm03-20260722T044823Z`
- Manifest SHA-256: `cc3b697bc6879cb5cbad00f83b01c3f4d0379148145a8084daac501a2c8f9c57`
- Subject: `latest-async-parallel`
- Base commit: `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Formal ordinals: 6-10
- Receipts: 5/5, each `attempt=1`, each `COMPLETE`
- Process exits: 5/5 publisher `0`; 5/5 subscriber `0`
- Admission gate: baseline receipts 5/5 before ordinal 6

| Rate | Scheduled | Attempted | API-completed | Delivered | Attempted pps | Delivered/attempted | Sender-limited |
|---:|---:|---:|---:|---:|---:|---:|:--|
| 200 | 12000 | 12000 | 12000 | 12000 | 200.000 | 100.000% | no |
| 400 | 24000 | 23847 | 23847 | 23847 | 397.450 | 100.000% | no |
| 600 | 36000 | 36000 | 36000 | 36000 | 600.000 | 100.000% | no |
| 800 | 48000 | 47578 | 47578 | 47517 | 792.967 | 99.872% | no |
| 1000 | 60000 | 37593 | 37593 | 37593 | 626.550 | 100.000% | yes |

Raw path:
`results/spec131-svs-pubsub-commit-latency/spec131-confirm03-20260722T044823Z`
