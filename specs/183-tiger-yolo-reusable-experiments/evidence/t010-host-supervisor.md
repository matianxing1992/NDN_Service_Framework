# T010 bounded host process-tree owner

Date: 2026-09-08. Baseline f3f49dd2 plus this checkpoint.
N2 source progress; MiniNDN, native, model, SIF and GPU qualification NOT_RUN.

`runtime/host_minindn.py` uses the installed systemd system manager (245 on this
host), via root or non-interactive sudo. A random, exclusively recorded transient
service owns the maintained driver and descendants. RuntimeMaxSec and
TimeoutStopSec enforce lifetime independently of the Python supervisor; mixed
kill mode lets the driver receive TERM and unwind, then kills surviving members.
TimeoutStartSec bounds service setup. No workload retry, global kill or unrelated
unit stop is allowed. Unit Description must match the retained owner nonce before
an explicit stop. Existing finite-process ownership bounds/reaps systemd-run;
each systemctl operation has a ten-second timeout. Client wait includes the
runtime + cleanup budget and 15 seconds of margin, with a ten-second client
cleanup budget; these control allowances are additional to the service budget.

The existing wrapper now calls this owner. Its budget comes from the verified
profile (staging + startup + one request deadline; cleanupSeconds separately).
Only explicit prepared environment inputs and PATH/PYTHONPATH are passed through
`env -i`; inherited SIF bypass knobs and unrelated credentials are excluded.
Exclusive output is `<run>/host-minindn/supervisor/`. Owner/result documents and
new logs are 0600; environment values are not copied into the owner document.

The result retains the systemd client exit, unit state, cleanup observations and
membership of the systemd cgroup on hybrid/v2 hosts. Missing supported cgroup
mounts, unavailable state, foreign ownership or remaining members fail closed.
`processCleanup: CLEAN` means empty after enforcement; it does not imply graceful
application success or intact network cleanup. Qualification stays NOT_EVALUATED.

## Retained evidence

`Experiments/TigerCluster/results/spec183-host-supervisor-20260908/`:

| Artifact | Observation | Scope |
|---|---|---|
| normal/owner.json, result.json, driver.log | Exit 0; 0.163 seconds; both systemd cgroup hierarchies empty | Actual transient unit, tiny Python print command |
| timeout-tree/owner.json, result.json, driver.log | Runtime budget 2 seconds + cleanup 1 second; observed 3.629 seconds; systemd Result=timeout, main killed by signal 9, systemd-run exit 1; both groups empty | Actual SIGTERM-ignoring parent and forked setsid child; both recorded PIDs absent from /proc afterward |
| boundaries.xml / boundaries.log | 12 passed | Component identity/stop/deadline/remaining-member/refusal/reuse tests plus actual wrapper boundary with a supervisor double |
| final-owner.xml / final-owner.log | 1 passed (overlap) | Only the owner success boundary repeated after adding service-start timeout and terminal-state retention |

The two actual probes were performed before adding explicit log mode, input
validation, terminal-state retention and the service-start timeout. These
additions are covered at component boundaries; no repeated systemd execution
was needed to re-establish the unchanged runtime/stop deadline behavior.

## Remaining gate

N2 still requires the three registered MiniNDN selectors and actual cleanup of
the network interfaces/namespaces owned by that execution. An empty cgroup alone
does not establish network-resource cleanup or successful protocol completion.
N1 still needs one semantic host producer/validator over the same actual normal,
permission and exact missing-dependency runs. No SIF build or Tiger allocation
is authorized by these component process probes. Preserve the unresolved base
SIF read-integrity finding; do not rehash/retransfer it blindly.
