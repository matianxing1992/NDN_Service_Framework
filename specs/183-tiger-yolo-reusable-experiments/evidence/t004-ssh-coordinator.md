# T004 SSH coordinator checkpoint — 2026-09-07

Status: IMPLEMENTED; full candidate / Slurm / GPU qualification NOT_RUN.
Parent T004 remains open. T007 remains BLOCK. Top-level completion stays 2/17.

## Implemented path and boundaries

`jobs/yolo/submit.py` retains original preparation/profile/harness and typed gate
checks. A same-path local project mirror enters `runtime/yolo_ssh.coordinate`;
the actual Tiger receiver enters the existing `_submit_shared` owner. The hidden
receiver flag is an internal routing argument, not a gate bypass. Sender tests
verify ordering and that no local submission journal is reserved.

The coordinator sends only the small hash-bound frozen harness/control packet
through authenticated SSH stdin. It invokes the configured Tiger Python with
shell-quoted arguments; the invoking host interpreter remains local. Receiver
preflight checks actual `ClusterName = itiger` and operator dependency pins.
The explicit content inventory binds original absolute paths; no receipt editing,
path rebasing, broad recursive copy, credential logging, or remote build occurs.

Payloads use system rsync with partial/append verification, protected arguments
and 0600 staged-file modes. Deterministic project `.incoming/<digest>` directories
retain interrupted data. The receiver independently verifies hashes, sizes and
modes, publishes without overwrite under managed-namespace exclusion, and seals
exact frozen harness directories before execution. Complete corrupt staged blobs
are retained with a rejected suffix; different final bytes are refused.

Same-run retries with fully matching sealed final contents perform no publication
and may reach the existing unknown-job query logic while its journal is active.
The frozen submit entry remains the sole sbatch owner. Observation timeouts retain
`REMOTE_STATE_UNRESOLVED`, never assert termination, and never authorize another
submission. Sender attempts and receiver submit observations are durable files.
`storage.transferTimeoutSeconds` defaults to 1800 (range 10..86400), distinct from
allocation stagingSeconds. The harness now includes 27 files. Actual E/profile
was not rerendered and no old E qualification is silently reused.

## Verification and retained evidence

Local result root: `Experiments/TigerCluster/results/spec183-ssh-coordinator-20260907/`.

| Evidence | Result | Actual scope |
| --- | --- | --- |
| `first.xml`, `first.log` | 1 passed / 14 fixture setup errors | New fixture inherited group-writable default modes; explicit declared modes fixed before rerun |
| `ssh.xml`, `ssh.log` | 15 passed, 6.76 s | Filesystem publication/sealing/reuse; bootstrap rejection and real frozen import; transport-list/timeout doubles; real local rsync prefix resume/mode |
| `affected.xml`, `affected.log` | 82 passed, 20.05 s | Existing shared-submit, frozen-bundle, CLI and candidate-inventory boundaries; scheduler doubles, no actual job |
| `sender-entry.xml`, `sender-entry.log` | 2 passed, 0.39 s | Gate→inventory→SSH ordering and no local sbatch/journal on success or timeout |
| `reply-types.xml`, `reply-types.log` | 5 passed | Final sender-only malformed path-type rejection; whole reply rejects before the first rsync, with an unresolved observation retained; overlaps earlier sender tests |
| `remote.json`, empty `remote.stderr` | Process exit 0; first and retry STAGED / NOT_EVALUATED | Actual SSH/rsync and independent login-node verification, 31 files / 507380 bytes, first reused 0; same manifest/stage on retry |
| `structure.json` (text report) | Structural PASS, 18 FR / 6 SC / 17 tasks / 2 complete | Document structure only, no semantic release PASS |

Actual remote/local fixture root:
`/project/tma1/ndnsf-di/candidates/spec183-ssh-probe-20260907a`.
The local `/project/tma1/ndnsf-di/candidates` mirror was created for this small
isolated fixture; no existing host qualification or source seal was relocated.
The fixture contains a synthetic text payload, profile, preparation and real
frozen Python harness. It does not contain a SIF, model, production secret or
claimed runtime receipt. Internal `submit=False` stops before any sbatch owner.

Actual command:

```text
/usr/bin/python3 -B Experiments/TigerCluster/tests/ssh_transport_probe.py \
  --root /project/tma1/ndnsf-di/candidates/spec183-ssh-probe-20260907a \
  --operator-python /project/tma1/ndnsf-di/operator-envs/py39-3b4f62bc/bin/python
```

Transport manifest SHA256:
`ae187488017bd5dda94cb683c18aa4274b2042f6ddef00b4ccd7e47d86117217`.
Frozen harness SHA256:
`4f501307bea53cd4b34c5b90ac7bf405f0f7e4b117155fafb8e44e992553bc85`.
Stage suffix: `8b7ce60236de2a81d314a9407647808d775a0b19c7b4ec5448ba893ad5a16a51`.
That remote snapshot precedes the final sender-only path-type rejection guard.
Its exact source identity is retained; that narrow guard was checked locally
without repeating the already observed 31-file transfer.

No native build, SIF read/rebuild, model execution or Slurm submission was repeated.
The old other-client build PIDs 1504838/1505475 were absent when inspected; their
build trees were left untouched. No new build was launched from that observation.
Next: wire the registered negative-dependency owner, re-audit current production
semantics for T007, then obtain the still-missing actual candidate runtime gates.
The prior base-SIF read-integrity finding remains unresolved by this work.
