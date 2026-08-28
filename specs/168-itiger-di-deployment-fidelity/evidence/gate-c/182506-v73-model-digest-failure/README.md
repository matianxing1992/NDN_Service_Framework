# Job 182506: model binding rejected

This explicitly labelled Gate C binding-correction attempt passed source-bundle verification, Repo ABI closure, NDNSF ABI closure, and full overlay import. The exact-SIF CUDA preflight then rejected a copied historical model digest before campaign admission.

- submitted: `sha256:a317ec50b68c46f15d4490748317dba81684c7a571ee7a079680048cce8fb60f`
- stage manifest and reference: `sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db`
- Slurm result: `FAILED`, exit `1:0`, elapsed `00:00:08`

The next candidate is a binding-only successor. It machine-reads and cross-checks the immutable artifact bindings before submission; it does not rebuild or reprepare source, SIF, model stages, schedule, or Repository payload.
