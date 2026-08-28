# Successor Matrix Contract

Spec 152 `contracts/rate-sweep.md` is incorporated unchanged:

```text
rates = 10,20,30,40,50,60 fps
warmup = 5 s
measurement >= 60 s
loss/reorder = 0
all other factors identical
rate error <= 5%
delivery >= 98%
future hit >= 95%
p99 <= 1000 ms
longest gap <= 1000 ms
Mapping = 0
ready/gap queues = 0
```

The only implementation deltas are single-thread low-latency H.264 decode and
a five-second bounded wait for the already-required asynchronous stop marker.
