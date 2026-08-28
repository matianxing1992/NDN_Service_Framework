# Gate E job 182391 - source-overlay import failure

- Campaign: `spec168-campaign-v3-f7cab32e087fefc1e4b5` (closed; never retry).
- Request ID: `spec168-f7cab32e087fefc1e4b5-single`.
- Slurm: `CANCELLED`, elapsed `00:04:43`, nodes `itiger[07-09]`.
- Source: `sha256:9b936cf5bde4b1dd7f0d55cfd885050a8f9ce07d159ee5a5ea16fb7956e31ecb`.
- Source bundle: `sha256:c71ad8c936c5089862e467e89a28b628f45fe2027ccd20004c7722469a33c3b3`.
- Campaign manifest:
  `sha256:7ae773bcc0a82b4d767c2c6c7169be07e7a4b78394f21917572eac1c84a91cd4`.

The v36 Repository-policy repair passed remotely: the Controller generated
seven bootstrap identities for three compute Providers, three Repository
Providers, and one User. All three Repository processes started. No User
Request was sent.

Rank 0 then executed `build-automatic-planning-manifest.py`. The Spec 162
compatibility kernel had prepended `/opt/ndnsf-app/python` ahead of the Spec 168
copy-up overlay, so the helper imported the stale SIF package and failed because
`build_qwen_three_stage_adapter` was absent there. Ranks 1 and 2 were waiting up
to 18,000 seconds for the missing planning marker. Once the deterministic
pre-Request failure was retained, job 182391 was cancelled to release three
GPUs; cancellation is part of the negative result, not a retry.

Slurm's final kill prevented the batch EXIT trap from completing. The retained
partial directory was therefore scrubbed without reading secret contents: two
selection keys and `bootstrap-tokens.txt` were deleted, `security-scrub.txt`
records `removedSecretFileCount=3`, and a subsequent scan found zero retained
key/token/private-key files. The directory was then atomically renamed to the
closed `FAILED-job-182391` identity.

The linked repair keeps a caller-supplied overlay ahead of the installed SIF
package and propagates any rank failure through a shared abort marker so other
barrier waits terminate promptly.
