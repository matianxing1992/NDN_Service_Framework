# Job 182518: v88 Qwen3-0.6B single request PASS

Campaign `spec168-campaign-v3-5aa1097fd99b29e0d4f8` was submitted exactly
once as Job 182518 on `itiger07`-`itiger09`. Slurm completed the job with
`0:0` in 3 minutes 21 seconds; the three-rank step took 3 minutes 11 seconds.
The closure analyzer recorded `PASS`.

The run proves, for this frozen source/SIF/model/workload identity:

- three distinct RTX 5000 Ada GPUs and three Provider roles;
- one durable request ID and one wire request (`tokenRequestCount=0`);
- Request-first ACK collection, automatic planning, three Provider-specific
  Selection projections, role-local preparation, and data-driven execution;
- three content-addressed DistributedRepo fetches with zero retransmitted
  bytes: Stage 0 fetched 594,357,850 bytes/78,205 Data in 7,691.46 ms, Stage 1
  fetched 283,192,711 bytes/37,263 Data in 17,071.13 ms, and Stage 2 fetched
  625,825,930 bytes/82,346 Data in 24,394.44 ms;
- CUDA preparation on all three ranks with load times 1,776.76 ms, 1,053.71
  ms, and 1,869.08 ms and zero CPU fallback;
- 47 generated tokens, EOS termination, and the complete Chinese answer
  explaining NDN name-based versus IP address-based forwarding;
- three mapped-native and three compatibility-child ABI checks matching the
  frozen core and Python extension digests.

This run closes Job 182516's registration-visibility defect: ranks 1 and 2 no
longer fall back to local pre-split hardlinks. It is a cold single request and
does not by itself prove the expected speedup from retained GPU residency; that
requires a separate two-request cold/warm campaign using the same source and
assets.
