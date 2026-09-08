# Layered local startup, 2026-09-08

Status: signed preparation and corrected Controller/Repo registration executed;
full four-Provider inference and formal qualification remain open.

## Protected Repo ACK source fix and actual STATUS response

Repo now registers the existing authenticated ACK-context API. In required
RequestScopedConfidentialityV1 mode it advertises service/capacity only,
without decoding input or claiming `hasObject`/`hasManifest`. Nonempty
pre-Selection input and unregistered services are rejected. The normal
post-Selection handler still checks the operation/service binding and
requester ownership. Legacy plaintext fixture behavior retains its decoder.
No Core protocol, confidentiality, replay or permission checks are disabled.

`repo-protected-ack-source` runs NFD+Controller+Repo+User using base ccdd/app89,
with an explicitly recorded one-file diagnostic Repo override SHA
`9f4c82033d5f865e6417e2f4f2570380215d525b9196fda9cd8eee8e10c2fe13`.
Its actual `user/requests/repo-readiness/receipt.json` has status READY,
attempts1, the expected Repo identity and probeId. User exits0; all four
processes are reaped without forced cleanup. This snapshot contains the ACK
fix before the subsequent client selector changes. It proves the original
STATUS timeout is resolved at the source seam, not new-SIF or YOLO qualification.

Client manifest lookup now treats absent object-presence fields as unknown,
tries each eligible Provider once within one total deadline, and establishes
presence from the selected Provider's response. It verifies the returned
object name. Default deletion discovers the finalized manifest first, then
uses its replicas (or the responding Repo if the list is empty), preserving
the existing explicit-replica deletion path. This removes the remaining
selectors that assumed plaintext was available during protected ACK.

The protected-ACK regression fails before implementation, then passes with
real native ACK conversion and post-Selection wrong-operation rejection.
The HA suite has53 distinct checks:52 pass initially, one new test incorrectly
compares an unnormalized manifest with a decoded canonical manifest. Correct
that expectation to the existing codec's normalization; its isolated rerun
passes. Selection tests exercise first-Repo miss/second-Repo success, confirmed
replica deletion, and exhaustion without repeatedly selecting the same Provider.
These use a request transport double, not distributed runtime evidence.
All tests use packaged native libraries with only the changed Repo Python
source mounted; no C++ compilation or SIF rebuild has occurred for these fixes.

Next: seal and repack the complete Repo Python changes once, bind the unchanged
application to the new base provenance, and run the full local CPU chain.
The formal MiniNDN gates and Tiger four-Provider campaign remain incomplete.

## Latest checkpoint: repacked composition, run c

`layered-host-20260908c` uses base `ccdd4ac0` and app manifest `89c49f7a`
(full identities in `base-python-repack.md`). Candidate SHA is
`28accac6c1787847193081a66cdf41ac96abeea905a03096afd21fdeed77315f`;
signed preparation receipt SHA is
`0dc826dc503879b88e75b0502cef06950fc221f7c1f9468d0ace0bdf57ce56fd`.
The unchanged frozen harness is `1227fded`. Actual preparation succeeds;
the complete local attempt exits2 at `APP_EXIT:user-repo-readiness:2`.
Controller catalogue publication succeeds and the prior NFD timestamp
registration rejection is absent. Repo STATUS still times out. All nine
owned operations are reaped and leases released without forced cleanup;
no inference request is accepted. This is not runtime qualification.

Minimal NFD+Controller+Repo+User reproduction `repo-status-multicast` uses
that same composition and role-isolated copied homes. Requests reach Repo,
decrypt and pass permissions; User reports `no_selection_published` and
times out without a readiness receipt. All four processes are reaped without
forced cleanup. Replayed requests are rejected; this alone does not establish
the cause of the missing ACK. The earlier `repo-status-current` reduction
omitted the group multicast strategy and is excluded as an equivalent setup.
`repo-status-timeline` repeats the symptom and clean teardown; its trace
configuration did not expose ACK stages, so it does not locate the fault.
Use bounded ACK stage instrumentation before changing deadlines or Core.

`repo-status-timing-r3` enables the TimelineTrace logger, control timing and
sample rate1. Earlier probes omitted that separate logger (and initially the
sample rate), so their absent stage messages were instrumentation gaps.
The first ACK handler starts at1788871652.098839 and finishes at
1788871652.100062: about1.2ms. It is then suppressed with TypeError because
`AckDecision(False, "repo-bad-request")` puts the string into the second
dataclass field, `payload: bytes`, instead of `message`. All five Repo negative
ACK branches now use named fields. A real SIF binding regression first fails
on the wrong payload, then passes `_to_native_ack` after the fix. Only the
changed Python file is mounted for this diagnostic; the packaged SIF is not
yet updated. The first test harness mounted a source-only package and failed
to find its native extension; the corrected one-file mount uses the packaged
extension. No C++ build or image repack was needed.

The underlying `repo-bad-request` is a second source-proven contract mismatch:
User logs `NDNSF_REQUEST_SCOPED_DEFAULT`; Core's
`prepareRequestScopedRequest` clears Request payload and retains input for
the selected Provider, while Repo `_ack` unconditionally JSON-decodes that
payload before Selection. Fixing argument order only makes rejection
observable; it does not restore STATUS success. Next, adapt Repo's protected
ACK path to authenticated service/capability metadata, retain operation/input
validation after Selection, and audit object-presence-dependent selectors.
Do not disable request-scoped confidentiality or lengthen timeouts to bypass
this mismatch. The timing run exits2 and reaps all four processes without
forced cleanup. Full CPU/MiniNDN/Tiger qualification remains open.

The development coordinator also had an independent pre-run audit defect:
it treated an existing request-output directory as a file and recorded
acceptance before validation. It now calls the production graph-reference
and result validators before acceptance, retaining the actual output files.
Three focused tests cover both validator failures and successful dispatch.
The frozen harness, base and app bytes are unchanged by this coordinator fix.

## Prepared composition

Run `layered-host-20260908a`, case `local-cpu`:

- Base SHA: `d4031191aed0e9aaa105032ffa4f5fa289b38044da29d06e261db701430f1dc6` (RAM snapshot).
- App manifest SHA: `c086d64c837b3561851aceb4c106b2512fd064ef60ba3a4624d2c249cc3fcff5`.
- Candidate SHA: `e3f670ce3c71710f4a52b739c48ba6618f18568abf97ed2776340f3105bfd642`.
- Preparation receipt SHA: `464057014d4730d69f450da1eb25c99fdd8494faa4fc3903f93bdaa591b2506f`.
- Frozen harness SHA: `1992b0a2f1913b99d16ea4f0a35171156a4ad8d01f0db2c64e6238592518d061`.

The profile explicitly declares local Apptainer1.5.3 at `/opt/apptainer/1.5.3/bin/apptainer`
and Tiger Apptainer1.3.4-1.el9 at `/usr/bin/apptainer`. Case selection changes
the declared tool, not base/app bytes. Issuer and retained rank validation use
the same selection. The development entrypoint rejects undeclared CLI locators
and uses the canonical prepared-run decoder. Actual signed provisioning exits0.

## Real failure and bounded fix verification

The initial local run exits2 with `CHILD_EXIT:controller:134`, after READY,
with `Fetched public parameters cannot be authenticated: Validator/policy did
not invoke success or failure callback`. No requests were accepted; all child
processes were reaped and leases released, without forced cleanup.

Root launch defect: `container_command` sets `--home /identities/controller`
but does not mount its issued identity outside preparation mode. A real
read-only probe in the same SIF reports PIB=false/session=false. Adding only
the controller-directory bind reports PIB=true/session=true. Both probes
report root identity absent. Ordinary runtime now mounts only its role HOME
read-write, preserving issuer-only access to the complete private directory.

The role-mount regression assertion fails before the fix; baseline/worker/
provision/version checks then pass: 116 tests in7.65s. The first test invocation
omitted the repository runtime PYTHONPATH and failed import; the corrected
invocation demonstrated the intended assertion failure before implementation.

A bounded NFD+Controller run uses copied role homes, the same base/app/public
inputs, fresh outputs, and the corrected launcher. An initial reduced harness
omitted NDNSF_DI_STATE_ROOT and failed RuntimeJournalUnsafeRootError; restoring
the production environment fixes this harness omission. The corrected run
`controller-identity-diagnostic/mounted-state-startup` generates the signed
runtime-publication receipt and remains alive until owned cleanup. Controller
exits143 on requested TERM, NFD exits0; both reaped, neither forced. The original
authentication abort is absent. No model, inference request, build, or Slurm
job was needed for this diagnosis. This is startup evidence, not a YOLO PASS.

Raw evidence: `Experiments/TigerCluster/results/yolo-layered-20260908/qualification/`,
under `layered-host-20260908a/` and `controller-identity-diagnostic/`. Private
identities remain ignored. Preserve the original failed run.

Next: refresh the frozen harness into a new candidate and run the complete
local CPU chain. N1/N2 MiniNDN semantic gates and the four-Provider Tiger
campaign remain open. This local launcher defect does not contradict the
delivered MiniNDN results from the other machine.

## Next full attempt and Repo command ordering

`layered-host-20260908b` freezes the role-home fix into harness
`1227fded39ce4d9df56732a89892c6be5ad484ba88aeec799982b5d3fac4f93f`;
candidate `9499c18c586aa767630c88380848395d9d5b152c2f38a0c7aacb960b5a28374f`.
Signed preparation exits0, receipt
`11c2f8ec7cf1900a84051b8f778a29b9802bf72908a81d324c04a0b4712ad166`.
I/R plane identities and base/app bytes are unchanged. The new dispatch plane
is under `/dev/shm/spec183-sdk-d4031191/planes-role-home`.

The full attempt passes Controller publication, then exits2 with
`APP_EXIT:user-repo-readiness:2`. Repo reports NFD `authorization rejected`
for its NDNSF/CK/NDNSF-DI/application prefixes. No inference requests were
accepted. All nine recorded child/finite operations are reaped, every lease
released, no forced cleanup. This is a new failed startup, not a CPU PASS.

Bounded NFD+Controller+Repo diagnostic `repo-management` exposes NFD's precise
reason: `Timestamp is reordered for key .../repo/KEY/...`. The Provider queues
its signed registrations during construction; RepoNodeApp.run starts another
Face with the same identity before starting the Provider event loop. Newer
commands arrive first and invalidate the queued timestamps under replay policy.

Changing only startup order in diagnostic `repo-provider-first-r2` eliminates
these rejections. NFD records successful routes for Repo KEY, NDNSF, CK,
NDNSF-DI, application prefix, sync and REPO-SERVING. Repo has no error/traceback;
all three processes are reaped without forced cleanup. The earlier
`repo-provider-first` invocation inserted Python -c at the wrong argv position
and failed argument parsing; it is retained and excluded from validation.

The production fix belongs to the existing Repo library owner:
`NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`. Start the
Provider after installing handlers, then advertise/start the data plane; the
subsequent blocking run remains idempotent. Enclose startup in cleanup and stop
the Provider on every exit. The order regression fails before the fix. The
existing HA suite passes48 tests; an added startup-failure check then passes
with the order check (2 selected tests, covering Provider/advertisement/data-plane
failures and resource cleanup). No replay policy or trust configuration changes.

The diagnostic changes Python in memory only and is NOT an immutable runtime
qualification. The retained d403 base still contains the old Repo Python.
NEXT: package the corrected library into a new base identity, reusing unchanged
native libraries and consumer binaries only after proving unchanged ABI/source
boundaries; verify the new exact base+app composition, then resume full CPU and
MiniNDN runs. Do not ship a host-library overlay or relabel the old base as fixed.

Pre-repack comparison against the original base's117 sealed workspace files:
116 unchanged; only `NDNSF-DistributedRepo/pythonWrapper/py_repoclient/orchestration.py`
changes, from `2cc685017ee7f33a2f7ef819db2088c805121c87f22a5c496f84a35d23fe6095`
to `abc43042bac8198eb12ff65291b354e491c1f27cceb2e7dd7b3730ce669c0487`.
This supports a Python-only base repack, not a cold native rebuild. The eventual
new image must independently retain the same native artifact hashes and pass
imports/closure plus corrected Repo runtime verification.
