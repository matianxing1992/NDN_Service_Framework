# Controller Contract

1. `observeRtt()` remains the direct known-produced network-delay input.
2. A new internal payload-delay observation distinguishes direct observations from ahead-mapped effective delays without changing the public Stream handle API.
3. Ahead-mapped effective delay cannot raise network RTT during Chasing/Adjusting; stable Fetching restores normal adaptive updates.
4. Chasing, Adjusting, and Fetching cannot mutate phase/window before the detection hold expires.
5. Adjusting restores the previous usable window when a reduction first causes instability.
6. `futurePayloadInterests` remains source-compatible but means ahead of the immutable join checkpoint; Provider future counters prove actual future admission/hits.
