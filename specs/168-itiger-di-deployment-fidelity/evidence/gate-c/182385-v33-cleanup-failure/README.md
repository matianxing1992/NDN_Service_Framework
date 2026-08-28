# Gate C job 182385 - CUDA pass, batch cleanup failure

- Slurm: `FAILED`, exit `1:0`, elapsed `00:01:44`, node `itiger07`.
- Source identity: `sha256:dc709220ea6db6cec8636ecfdb6d910693eb446c416e252cfa9a4bdf01e0b779`.
- Source bundle: `sha256:b6559af64026d0a64f911438206d4fb54e5629207cdd1c941f46f1fec0b5c293`.
- Exact SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Qwen3-0.6B stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.

The source/mode verifier and complete copy-up import passed. All three existing
stage artifacts loaded, warmed, and executed on `cuda:0` without CPU fallback;
the expected and actual top token were both 8065. The formal job nevertheless
failed after inference because the host wrapper attempted to delete the
read-only copy-up overlay. Therefore this identity is not a Gate C PASS.

Replacement v34 moves deletion into the container namespace after restoring
owner write permission. No model, SIF, planning, inference, or Repository
behavior changed.

Retained SHA-256 values:

- `preflight-result.json`: `55ae88ca9cfec8141ec286c26baa29b6145670f4527b93daf5f8179b766c6f95`
- `slurm.out`: `72480100992a53f0f39b4d903604231c10164d45737b50534f2d28b880d77c11`
- `slurm.err`: `e39b1b63461a69a1bb40caeae8d6662dc582b7d5098ee95e1aa013c9cd3df4ff`
