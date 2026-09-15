# Data Model: YOLO MiniNDN SIF+APP Fast Path

## BaseSif

| Field | Rule |
| --- | --- |
| path | regular file, no symlink components |
| digest | SHA-256 recorded before and after every stage |
| runtime contract | /opt/ndn-base/lib, /opt/onnxruntime/lib and /opt/venv/bin/python exist |
| ownership | stable runtime only; no versioned DI APP |

## YoloApp

| Field | Rule |
| --- | --- |
| root | immutable, no symlink components |
| native closure | eight declared APP libraries; RPATH resolves APP or base only |
| Python closure | extension and interpreter are built/verified in the container boundary |
| entrypoint | maintained Experiments/NDNSF_DI_YoloAckDriven_Minindn.py path |

## PairManifest

Binds source seal, definition digest, base digest, APP digest, build record, Apptainer identity, profile, model/input identities, mount table and validation contract. Any change invalidates later evidence from the earliest affected gate.

## RunRecord

Contains candidate identity, exact command, C++ selector, MiniNDN/NFD/KeyChain inputs, child exit statuses, terminal oracle, cleanup barrier, first failure boundary and status. Local and Tiger records are separate entities.
