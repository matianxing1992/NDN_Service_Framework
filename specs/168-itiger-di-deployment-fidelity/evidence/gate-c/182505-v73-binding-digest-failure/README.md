# Job 182505: stage-manifest binding rejected

This Gate C admission attempt failed closed before creating an output directory because the submitted expected stage-manifest SHA-256 was copied incorrectly. The source bundle itself passed verification. No SIF execution or formal inference campaign occurred.

- submitted: `8d8475db557397fabfff547bd7041c741bed254beb84585085924193562d1490`
- immutable artifact: `8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`
- Slurm result: `FAILED`, exit `1:0`, elapsed `00:01:31`

The failure is retained as operator-binding evidence. It is not an NDNSF-DI, CUDA, SIF, or model-runtime failure.
