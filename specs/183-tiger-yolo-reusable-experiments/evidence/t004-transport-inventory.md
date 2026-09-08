# T004.p — candidate and prerequisite transport inventory

2026-09-07. Read-only inventory is wired into public `submit --plan-transport`.
T004 stays partial: automated SSH coordination/receiver execution is still open.

The public path validates dispatch/profile/preparation/frozen harness and the
required prior gate before planning. It verifies any other declared gates too,
resolves publisher inputs through the existing provision mapper, then enumerates
the exact files. Output is PLANNED/NOT_EVALUATED with the transport manifest;
exit 78. Local frozen-Python reentry may occur, but no SSH, Slurm, model execution
or publication does. The flag is forwarded into the frozen CLI.

Inventory includes profile, all three plane documents and their listed files,
E-plane and run harnesses, explicit issuer public inputs and the one declared
authority key, canonical ONNX/weights/oracle, and retained prerequisite closure.
The latter includes prepared/collection/verdict records, node launch logs,
per-request graph/lifecycle/numerical/response/assignment files, observed ORT
profiles, reference package/oracle/fixture, host source-seal/evidence and GPU
allocation/probe/srun/storage/terminal records when applicable. It consumes
validated collector results instead of copying all result directories or parsing
logs with a second protocol implementation. Relative collection locators resolve
against their original run root; every selected path must remain in the declared
same-name project namespace. Historical receipts are never rewritten.

Referenced file identities, model graph/weights, harness files, host source seal,
oracle/fixture and node log bytes are checked against expected digests while
creating the manifest. Missing/changed/out-of-namespace files fail. Only the
declared authority key is marked private; role homes and .ndn key databases are
not enumerated. Receiver still verifies all incoming contents and reruns normal
submission qualification. A file manifest alone cannot manufacture a PASS.

Frozen harness inventory grows from 25 to 26 files by adding
runtime/yolo_transport.py, which the frozen submit planner imports. The actual
profile/plane snapshot has NOT been rerendered; earlier E qualifications cannot
stand in for this changed harness. No extra SIF build was started.

Evidence root: results/spec183-transport-inventory-20260907.
- focused.xml: 80 passed in 19.84 s (inventory, bundle, CLI, prerequisite reuse).
- final-boundaries.xml: 21 passed in 1.69 s (gate-before-plan, no scheduler path,
  frozen flag propagation and interpreter tests).
- final-inventory.xml: 9 passed in 1.68 s after adding host-source digest binding
  and host/GPU-specific enumeration coverage.
Groups overlap; do not sum. Filesystem fixtures supply synthetic receipt shapes;
boundary tests explicitly double qualification. These are not real YOLO PASSes.
No test failure occurred. No current real candidate transport manifest was made:
formal prior gates, project layout and base-SIF read integrity remain unresolved.

Other-client progress was verified from Git/process state: 4ade12bf/2f5e4992
record clean-root unit/integration binary RC0; 1512b203 adds the T010 driver.
At inspection, build-system-j2 had a live -j2 waf/cc1plus build. It was left alone;
neither these binary-suite results nor driver source proves T009/T010 YOLO runtime.

Next: bounded authenticated SSH coordinator, invoke guarded receiver, independently
validate destination closure then enter the existing sole sbatch owner. Preserve
submission-unknown recovery; do not retransfer or resubmit on an observation timeout.
