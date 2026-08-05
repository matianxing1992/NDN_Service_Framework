# Spec 163 bounded local Docker lifecycle

Run:

```bash
tests/container/placement-preparation/run.sh
```

The host wrapper reuses the sealed local NDNSF-DI runtime image and the current
workspace build. It creates one container with `--memory=4g
--memory-swap=5g`, starts one NFD, and runs the real Controller/requester/three
Provider security carrier plus the byte-payload DI V2 deferred lifecycle. It
does not load Torch, Qwen, CUDA, or model weights.

Output is retained under `results/spec163-local-docker-<timestamp>/`. A run is
accepted only when `gate-summary.txt` ends with
`SPEC163_LOCAL_DOCKER_ALL_GATES_PASS` and `docker-inspect.txt` records the exact
memory limits, zero OOM kill, and exit code zero.
