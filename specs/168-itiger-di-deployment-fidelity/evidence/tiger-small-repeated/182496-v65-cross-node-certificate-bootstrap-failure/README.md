# Job 182496: cross-node certificate bootstrap failure

Status: **FAILED (operator-terminated after deterministic diagnosis)**.

The frozen v65 campaign reached three positive Provider ACKs, generated and
published the three content-addressed stage artifacts, committed Selection,
and let Stage 0 fetch 594,357,850 bytes and load its Qwen stage on CUDA.  Stage
1 and Stage 2 never accepted their provider-projected Selection.  Their
independent PIBs did not contain the User certificate, while the User PIB did
not contain the Provider 1/2 certificates needed to validate signed
`SELECTION-STATUS` Data.  Provider 0 progressed only because it shared rank 0
with the User.

This is a deployment-fidelity defect, not a Qwen or DistributedRepo throughput
result.  The MiniNDN gate used same-host security state and asserted eventual
response behavior, but did not require an isolated-PIB certificate matrix or a
bounded early-ACK closure.  Gate B also took about 126 seconds with a 120-second
ACK timeout, which should have been a failure rather than a pass.

The allocation was cancelled after 12 minutes 32 seconds to avoid holding
three GPUs until the one-hour request deadline.  Jobs 182497 and 182498 were
non-GPU evidence-recovery allocations that copied the still-present per-rank
logs from node-local scratch.  Bootstrap tokens and selection keys were removed
before this evidence was retained.  This campaign must never be relabelled or
retried under the v65 source identity.

Required regression before another formal campaign:

1. Use an independent HOME/PIB for every rank in the local/container gate.
2. Export and install every Provider public certificate on all participating
   ranks before the request gate opens.
3. Export the User public certificate after `APPClient` construction and block
   request publication until remote Provider ranks have installed it.
4. Reject any User/Provider local-PIB certificate lookup miss for Request,
   Selection, stage Data, or `SELECTION-STATUS`.
5. Require ACK coverage closure soon after the third valid ACK; eventual
   success at the full 120-second ACK timeout is not a pass.
