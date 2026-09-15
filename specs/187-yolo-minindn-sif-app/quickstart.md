# Quickstart: YOLO MiniNDN SIF+APP Fast Path

## Prerequisites

- A regular base SIF satisfying contracts/yolo-pair.md.
- Apptainer 1.5.3 and a current host-gate manifest.
- Sealed source handoff produced by prepare-development-handoff.py.
- YOLO model, profile, identity/KeyChain and NFD inputs in an operator-owned run directory.
- Enough local disk for base, candidate, APP snapshot and temporary build output.

For the native local gate, also provide absolute, candidate-bound paths through
`SPEC187_NATIVE_MODE=1`, `SPEC187_NATIVE_SELECTOR`,
`SPEC187_NATIVE_REQUEST_CONFIG`, `SPEC187_NATIVE_REQUEST_INPUT` and
`SPEC187_NATIVE_REQUEST_OUTPUT`. The selector is launched as the MiniNDN User
process and inherits its node transport; a missing input or an existing output
is rejected before MiniNDN starts.

## Local sequence

Run from the repository root and use a new candidate/run-id directory:

1. prepare and verify the source handoff.
2. render the runtime definition with the exact base SIF and host-gate manifest.
3. invoke build-local-sif.sh with Apptainer 1.5.3.
4. invoke build-sif-app.sh and validate the resulting pair manifest.
5. run the existing YOLO MiniNDN harness with the local pair and C++
   `Spec187YoloMiniNdn/NativeRequesterThroughMiniNdn` selector.
6. repeat the local case once with the same pair identity; retain separate run records.

Do not run steps 3–6 while the base SIF is a dangling link or while the candidate closure is not locally available.

## Tiger promotion

After two local runs satisfy LOCAL_PASS, copy the unchanged pair, manifest, profile and model/identity inputs. Run one bounded Slurm job through the same run-sif-app.sh entry. Revalidate all digests before the first request. Scheduler, Apptainer, NFD and KeyChain failures remain separate UNQUALIFIED or WAITING_EXTERNAL_INPUT records.

## QWEN

QWEN is TODO for this feature. Do not add its model, tokenizer or streaming state to the YOLO pair.
