# T005 signed cross-node readiness component

Date: 2026-09-07. Status: PARTIAL / actual NFD, SIF and Tiger execution NOT_RUN.

## Implementation

`apps/yolo_network.py` is a finite diagnostic command. After the two NFDs and
routes are ready, both workers must launch it concurrently with the same fresh
32-hex probe ID. Node 0 borrows BackboneNeck's private HOME; node 1 borrows
DetectShard0's. Neither real Provider may already be running. The shared finite
process owner retains HOME exclusion, bounded deadline, clean teardown and
non-overwriting invocation output. On success the lease is released before
native Provider startup; the probe does not mark a Provider as started.

Each side validates both leaf certificates against the pinned run root, signs
an exact `/identity/SPEC183-NETWORK/<probeId>/data` Data name, and verifies the
peer's signature, exact name and exact nonce/producer payload. Data freshness
is zero and Interests require freshness. An invalid signature is a failure,
not a timeout to ignore. Only Nack/InterestTimeout may retry within the fixed
monotonic observation window. The producer remains available for the full
window after obtaining peer Data so the other rank can finish its own fetch.
Late/missing peers fail closed. There is no Controller PUBPARAMS probe.
Provider identity names are passed from the prepared plan, not inferred from
role names; custom names within the run namespace are preserved.

The finite container is CPU-only, with its own HOME and public certificates;
there is no model mount, peer private HOME or filesystem activation transfer.
Receipt is written exclusively after the NDN app returns normally. The parent
`wait_network_ready` requires a prepared Worker, matching rank/mode, exact
fresh receipt, clean child exit, no symlink evidence and live owned services.
The operator still must require BOTH directional receipts before proceeding.
This demonstrates only the diagnostic component's contract, not permissions,
model assembly, CUDA or full inference readiness.

The frozen harness now contains 16 explicit required files, including the
real `apps/yolo_network.py`; missing `run.sbatch` continues to prevent release.

## Evidence and limits

13 new test cases. The in-memory Face uses actual python-ndn RSA Data signing
and root/leaf signature validation with freshly generated 2048-bit keys.
The paired exchange passes; corrupted signatures/certificates, wrong payload,
wrong name and missing peers reject with the specific expected failure type.
Separate tests execute real finite OS children behind the fake Apptainer
boundary, check no GPU/model/foreign HOME mount, verify HOME release and later
service startup, and reject repeated probes and stale/foreign/nonzero receipts.

Initial test defects were an undefined comprehension variable and the wrong
ValidationFailure constructor in the fake Face. Both were fixed; the negative
test now requires ValidationFailure specifically instead of accepting any
Exception (which could conceal a broken test adapter).

Full focused command:

```sh
python3 -m pytest -q Experiments/TigerCluster/tests \
  tests/python/test_spec183_v3_backend_selection.py \
  tests/python/test_spec183_public_recipients.py --tb=short \
  --junitxml=Experiments/TigerCluster/results/t005-network-readiness-r2/junit.xml
```

**402 passed in 25.00s** (final identity-binding revision). The preceding full
suite passed 401 cases in 30.12s. No native NFD/SIF/model/remote execution occurred.
Final worker coordination/startup barrier, full request collector, public
operator and T007 production audit remain incomplete; no task qualification
checkbox is promoted by this component result.
