# Operator CLI contract: `ndnsf-di-itiger-qwen`

## Safety defaults

- Every command is read-only/render-only unless `submit` or `cancel` is explicit.
- `submit` accepts only a frozen candidate and one unused submission identity.
- No command accepts a plaintext password, MFA code, private key, or access token
  as a CLI argument or writes one to logs/evidence.
- A failed run is never automatically submitted again.

## Commands

```text
ndnsf-di-itiger-qwen discover --output <cluster.json>
ndnsf-di-itiger-qwen release validate --manifest <release.json>
ndnsf-di-itiger-qwen release build-render --source <sealed-source> --output <job.sbatch>
ndnsf-di-itiger-qwen release materialize --oci <digest-ref> --project <root>
ndnsf-di-itiger-qwen storage admit --profile <candidate.json>
ndnsf-di-itiger-qwen network render --cluster <cluster.json> --output <job.sh>
ndnsf-di-itiger-qwen network submit --rendered <job.sh> --submission-id <id>
ndnsf-di-itiger-qwen candidate freeze --profile <profile.json>
ndnsf-di-itiger-qwen candidate render --candidate <candidate.json> --cell <cell>
ndnsf-di-itiger-qwen candidate submit --rendered <job.sh> --submission-id <id>
ndnsf-di-itiger-qwen status --job-id <id>
ndnsf-di-itiger-qwen wait --job-id <id> --timeout <duration>
ndnsf-di-itiger-qwen cancel --job-id <id>
ndnsf-di-itiger-qwen evidence collect --job-id <id> --output <dir>
ndnsf-di-itiger-qwen evidence validate --bundle <dir>
ndnsf-di-itiger-qwen aggregate --campaign <campaign.json> --output <report.json>
ndnsf-di-itiger-qwen cleanup --project <root> --dry-run
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | requested operation valid/successful |
| 2 | CLI/profile/schema misuse |
| 3 | preflight/admission blocker; no live-task completion |
| 4 | submission identity already used |
| 5 | executed negative candidate outcome |
| 6 | evidence incomplete or invalid |
| 7 | authority/secret/safety violation |

## Crash-safe submission journal

At-most-once submission uses a durable journal, not a receipt written only after
`sbatch` returns:

1. atomically create a unique journal with state `INTENT_RECORDED`, submission
   ID, deterministic Slurm job name/comment, script/candidate/cell digests, and
   timestamp before invoking `sbatch`;
2. invoke `sbatch --parsable` exactly once;
3. atomically transition to `SUBMITTED` with job ID and stdout/stderr digests;
4. if transport/process failure leaves `SUBMISSION_UNKNOWN`, never call
   `sbatch` again automatically; reconcile the deterministic job name/comment
   through `squeue` and `sacct`, then record `SUBMITTED` or
   `CONFIRMED_NOT_SUBMITTED` with evidence;
5. only a human-authorized new submission identity may follow a confirmed
   pre-start failure.

Any existing journal blocks a second automatic `sbatch`. Journal transitions
are append-only or atomic-replace with prior-state digest retention.
