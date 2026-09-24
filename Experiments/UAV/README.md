# Spec191 UAV tracking experiment

The canonical MiniNDN topology has exactly five network nodes:

```text
uav1 ─┐
uav2 ─┼─ compute ─ gs
uav3 ─┘
```

`uav1`, `uav2`, and `uav3` are the three drone camera/source nodes; `compute`
is the only processing/tracking/inference node; `gs` is the Ground Station.
The Controller is a separate process and identity inside `gs`, and the
compute-host renderer is a separate process inside `compute`; neither is a
sixth MiniNDN node. The manifest therefore has five network nodes and seven
application processes. Every node has its own NFD; no extra forwarder is
allowed.

`run_multicamera_tracking_demo.py` is the only Spec191 entrypoint. `--prepare`
creates a hash-bound manifest without downloading or copying the large local
assets. `--preflight` validates the offline candidate. `--run` starts bounded
process groups only after native binaries, root MiniNDN privileges, and the
requested display mode pass. Use `--headless` only for pipeline automation; it
does not satisfy the three-window desktop criterion.

The three source commands are bound to the real cached videos and the compute
command is bound to the pinned local model. Results must be collected through
validated NDNSF named Data; a path, process exit, or preflight JSON is not a
result. Runtime evidence belongs under the ignored run directory and the
Spec191 evidence files record whether each gate is implemented, executed, or
measured.
