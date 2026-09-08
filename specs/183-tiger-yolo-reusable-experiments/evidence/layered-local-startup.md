# Layered local startup, 2026-09-08

Status: signed preparation and corrected Controller startup executed;
full four-Provider inference and formal qualification remain open.

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
