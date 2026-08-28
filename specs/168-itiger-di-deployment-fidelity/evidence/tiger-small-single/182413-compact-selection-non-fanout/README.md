# Job 182413: compact Selection did not fan out

## Frozen outcome

- Slurm job: `182413`
- Campaign: `spec168-campaign-v3-0216204cf0b9bacae920`
- Source bundle: `sha256:576bd17903c8075219a5f51426a1c7663cfd4628f668e434932f0d0b9b5714ad`
- Terminal state: operator-canceled after 17:49 when the same no-progress
  boundary remained deterministic; this identity is failed and must not be
  retried.
- Remote archive: `/project/tma1/ndnsf-di/evidence/spec168/tiger-small-single/spec168-campaign-v3-0216204cf0b9bacae920-FAILED-job-182413`
- Secret scrub: four ephemeral selection-key/token files removed before the
  remote archive was copied; `raw/security-scrub.txt` is the retained proof.

## Last verified checkpoint

The User completed Request, three positive ACKs, `ACK_CLOSED`, graph/placement,
deferred Repository publication, and `commit_plan`. Stage 0 received Selection,
fetched its 594,357,850-byte shard in 6.109 seconds (78,205 Data packets, zero
retransmissions), loaded CUDA in 1.760 seconds with no CPU fallback, executed,
and published its first dependency output. Stages 1 and 2 never received their
LLM Selection and therefore correctly did not prepare or execute.

The observed stage-0 Repository rate was about 97.3 MB/s payload throughput;
Repository, model loading, and GPU capacity are not this job's primary failure.

## Differential diagnosis

The single compact LLM Selection contained three opaque assignment payloads of
2,019, 2,305, and 2,067 bytes. The Hybrid publish log reported 7,468 ciphertext
bytes before wrapped-key, TLV, signature, and SVS outer-packet overhead. Rank 0
observed the locally published LLM Selection and committed its projection.
Ranks 1 and 2 observed later, smaller DistributedRepo compact Selections but
recorded zero LLM Selection observations and zero User-certificate validations
for that message. Thus failure occurred before remote decryption, role matching,
model fetch, or inference.

The exact discarded outer packet size was not captured before cancellation, so
the narrow measured claim is an unbounded compact-control-message transport
failure, strongly consistent with exceeding the NDN single-packet budget. The
source had no protected-wire size guard. The repair replaces collaboration-wide
compact Selection with provider-specific authenticated projections while
retaining one request ID, one plan, one Selection phase, and one attempt.

## Evidence limitation

External `scancel` terminated the Apptainer namespace before its EXIT trap could
copy node-local logs. The archive therefore retains campaign identities,
planning/Repository records, Slurm output, and the manual terminal classification,
but not the live provider logs inspected before cancellation. This limitation
must remain visible; a replacement candidate must produce durable per-Provider
Selection markers before acceptance.
