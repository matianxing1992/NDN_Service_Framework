# T004.n — explicit inventory and no-overwrite receiver

2026-09-07. Transport component implemented; T004 and runtime qualification open.

`runtime/yolo_transport.py` inventories explicitly selected regular files under
operator-supplied artifact/run roots. The manifest binds absolute paths, sizes,
SHA-256, modes, declared private-file flags and caller candidate digest. It
rejects duplicates, path ambiguity, file/parent collisions and non-owner-only
modes for declared secrets. Caller still owns semantic closure and classification.

Receiver validates all required blobs/existing destinations before publishing,
measures statvfs capacity, hashes copying bytes through a private temporary file,
fsyncs, and uses no-overwrite hard links. Different existing bytes/modes fail.
Exact files are revalidated/reused. Failures keep incoming staging and remove
only their own temporary file. Final destination bytes are re-read. statvfs is
neither quota nor reservation. `tools/spec183_transport.py` is an internal CLI;
it never prepares, reserves a journal, submits or qualifies an experiment.

Focused command: `python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_transport.py
--junitxml=Experiments/TigerCluster/results/spec183-transport-receiver-20260907/final.xml`.
**15 passed in 0.74 s**: real filesystem publication, exact reuse without blobs,
interrupted second publication/recovery, corrupt last blob, content/mode conflict,
symlink, bad parent, manifest rejection, FIFO rejection and real CLI. Capacity
uses a zero-space fixture; interruption injects OSError at the second link.
First focused.xml retains an import-path collection failure; explicitly declaring
the Tiger test root fixed it. No larger/model suite was rerun.

Actual SSH exercise copied four canonical source files and two synthetic blobs,
no keys/models/SIF or real prerequisite, in a 11045-byte archive. Local/remote SHA:
`2e62905932993f6e670a215f456b72e8da582782987f8e183e141d158ad47de4`.
Remote root: `/project/tma1/ndnsf-di/candidates/spec183-transport-probe-20260907a`.
Installed operator Python ran the canonical receiver: first published 2 files,
reused=0; repeat reused=2, inode/mode/bytes unchanged. Both returned
CONTENT_VERIFIED/NOT_EVALUATED with manifest digest
`sha256:d3ad52d7d928335f2ce444c5c05ed452b30971b6d0dc275b0d0ac0965a86f1f8`.
The candidate digest is synthetic transport-fixture identity only.

Remote receive-first.json, receive-reuse.json, verification.json and incoming
manifest/blobs remain there. Local archive, tests and remote-verification.json
remain under results/spec183-transport-receiver-20260907. SSH/scp calls had 25–40 s
bounds, all exit 0. No Slurm allocation or native/model execution occurred.

Still required: automatic candidate/prerequisite file enumeration, exclusion of
active allocations, bounded SSH coordination/recovery and public submit wiring.
An explicit file manifest cannot prove semantic completeness. Large-SIF read
integrity and negative runner also remain open. No end-to-end transport PASS.
