# Job 182516: v87 Qwen3-0.6B single request

Campaign `spec168-campaign-v3-ed94ffa779ce8458c0c1` was submitted exactly
once as Slurm Job 182516 on `itiger07`-`itiger09`. The operator cancelled the
deterministically failed run after 11 minutes 28 seconds rather than leave
three GPUs occupied until the one-hour request deadline. It was not retried.

The retained shared files under `raw-partial/` prove source/SIF/model identity,
three nodes, three Repo processes, three Providers, automatic planning, the
single request gate, and a complete three-artifact Repo registration. Slurm
classified the job `CANCELLED`.

Before cancellation, read-only inspection of each rank's node-local log showed:

- the deferred publisher registered all three role artifacts after ACK
  collection and before Selection;
- one request ID, `spec168-ed94ffa779ce8458c0c1-small-single`, was projected
  to all three Providers;
- Stage 0 fetched 594,357,850 verified bytes in 78,205 Data packets with zero
  retransmitted bytes in 7,621.18 ms, loaded on `cuda:0`, and published its
  hidden state;
- Stage 1 and Stage 2 failed before Repo fetch with
  `pre-split Qwen artifact cannot enter the content-addressed cache without
  copying; publish it through DistributedRepo`.

Those node-local log files were reclaimed by Slurm after cancellation before
they could be copied into the shared evidence directory. The observations
above are therefore diagnostic operator evidence, not a substitute for raw log
files, and no formal throughput claim should rely on them alone.

Root cause: Providers late-bind `repo-registration.json`, but a rank that does
not yet observe that shared file silently falls back to its pre-split local
path. The fallback attempts a cross-filesystem hardlink into the
content-addressed cache and fails. Concurrent Selection retries also exposed a
transient empty registration map because refresh used `clear()` followed by
`update()`. The repair atomically replaces the registration snapshot and, when
DistributedRepo registration is configured, waits with bounded progress until
the selected role appears or the Selection assignment deadline expires. It
must never silently fall back to pre-split data in that mode.

The run also appeared to spend substantial time between Provider startup and
the user request. Job 182518 started its request much earlier, so the evidence
does not establish a fixed 300-second settle interval. The launcher timeline
must be inspected before classifying this as a separate efficiency defect.
