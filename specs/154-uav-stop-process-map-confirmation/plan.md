# Implementation Plan: UAV Stop Process-map Confirmation

Add `drone_processes = {}` next to `drone_logs`, store every handle returned by
`start(...)`, and pass `drone_processes[drone_id]` to the existing bounded
`wait_log`. Add source-contract and Python syntax tests that would have caught
the Spec 153 NameError.

Then freeze the unchanged six-rate matrix through a new wrapper and execute all
cells exactly once. No decoder, Core, prefetch, Mapping, FEC, retry, topology,
rate, duration, or acceptance threshold changes.

## Constitution Check

Current API/security unchanged; source ownership is launcher-only; Spec Kit and
frozen successor evidence are explicit; final verification remains two-node
MiniNDN with >=60-second windows; matched ARS experiment design is unchanged.
