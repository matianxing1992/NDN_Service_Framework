# Formal Matrix Contract

Incorporates Spec 152 `contracts/rate-sweep.md` and Spec 153 decoder repair
unchanged. The only new source delta is:

```text
drone_processes[drone_id] = drone_proc
wait_log(..., proc=drone_processes[drone_id])
```

The formal matrix remains 10/20/30/40/50/60 fps, zero loss/reorder, 5-second
warm-up, >=60-second measurement, immutable hashes, and no rerun.
