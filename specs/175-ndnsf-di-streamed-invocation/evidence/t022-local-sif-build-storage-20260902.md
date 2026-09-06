# T022 local exact-SIF build stopped by storage guard (2026-09-02)

This is a candidate-local packaging record, not a qualification pass.

- Candidate: `spec175-final-candidate-r5`
- Builder: local Apptainer `1.5.3`
- Command: `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`
- Result: stopped intentionally during final bootstrap extraction/post stage
- Reason: available host space fell below the 20 GB build guard (observed
  approximately 13 GB)
- The Apptainer builder removed its incomplete `.partial` output while
  unwinding after the stop; the candidate directory therefore contains no
  accepted SIF or build record from this attempt. The terminal build transcript
  and this evidence note are the retained failure evidence.

The container-native C++/NDNSF/NDN-SVS and Python extension builds completed
inside the SIF build boundary, including native import and `ldd` checks. The
failure is therefore a packaging-capacity stop, not evidence of a runtime or
NDNSF protocol defect. T022 remains open and must resume with the same frozen
candidate inputs after storage is safely reclaimed; no alternate matrix or
remote materialization is authorized.
