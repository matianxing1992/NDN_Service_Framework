# Invalid Spec 153 Campaign

Partial root:
`results/spec153-uav-decode-stop-formal-20260726T080534Z`

The 10-fps measurement completed and proved the decoder repair:

```text
achieved=9.994 fps
delivery=99.851%
p50=100.868 ms
p99=878.643 ms
```

Post-measurement launcher validation then raised:

```text
NameError: name 'drone_procs' is not defined
```

The same source path would deterministically invalidate every remaining cell.
The owned runner was terminated to avoid five scientifically useless
repetitions. No cell is accepted as formal evidence and the partial root is
preserved. Spec 154 adds an explicit per-drone process map, tests its
definition/use, and reruns the complete six-rate matrix in a new root.
