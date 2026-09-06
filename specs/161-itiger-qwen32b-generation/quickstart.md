# Quickstart: Spec 161 Validation

No command in this guide authorizes model download or a live Slurm submission.
Formal jobs remain exactly-once and require an explicit user-approved identity.

## 1. Local contract and generation tests

```bash
python3 tests/python/test_spec161_qwen_generation.py
```

Expected:

- 64-token bound accepted;
- input context grows by each selected token;
- EOS yields `OK`;
- token limit yields `TRUNCATED`;
- mismatch/failure rows remain evidence;
- warmup is excluded from measured percentiles;
- five prompts by five repetitions produce 25 measured records.

## 2. Spec structure and consistency

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/161-itiger-qwen32b-generation --strict

.specify/scripts/bash/check-prerequisites.sh \
  --json --require-tasks --include-tasks
```

Expected: required artifacts and sequential task IDs pass.

## 3. Read-only iTiger discovery

```bash
uofm-vpn-status
ssh -o BatchMode=yes itiger 'hostname; whoami'
/home/tianxing/.codex/skills/itiger-ndnsf-ops/scripts/discover-itiger.sh
/home/tianxing/.codex/skills/itiger-ndnsf-ops/scripts/discover-qwen-readiness.sh
```

Expected: account, node/GRES, storage, Slurm, and Apptainer facts are recorded.
This is discovery, not permission to compute.

## 4. Capacity decision

After implementation, run the Spec 161 durable preflight in read-only mode
against `/project/tma1/ndnsf-di`. It must emit `allowed=false` while
authoritative quota or the 20 GiB reserve cannot be proven.

The first authorized preparation allocation must then select and measure
temporary workspace in this order:

1. `$SLURM_TMPDIR`;
2. `/scratch`;
3. `/tmp`.

It must prove the selected path is allocation-local, writable, and large enough
for the full source-model download, H100 reference, stage construction, and
safety margin. Shared filesystem free space is not a substitute for this check.

Do not download 32B weights until:

- quota authority is recorded;
- current durable bytes, projected durable promotion, and temporary scratch peak
  are measured separately;
- the exact cleanup inventory, if any, is separately approved;
- `allowed=true` is checksum-bound into the candidate manifest.

## 5. Live gates after explicit authorization

Run in this order, each with a new frozen identity:

1. bounded H100 standalone reference and tokenizer/prompt freeze;
2. bounded stage preparation and per-stage CUDA load;
3. local/fake 64-token and five-by-five campaign smoke without 32B weights;
4. one three-node one-prompt complete-generation development smoke;
5. one exactly-once five-prompt campaign: one warmup plus five measured
   generations per prompt.

Stop and preserve evidence on the first failure. Never retry or modify a
measured identity in place.

After stage/tokenizer promotion and verification, remove only the current
preparation job's validated scratch prefix. Do not promote the reconstructible
full source-model copy or Hugging Face download cache to project storage.
