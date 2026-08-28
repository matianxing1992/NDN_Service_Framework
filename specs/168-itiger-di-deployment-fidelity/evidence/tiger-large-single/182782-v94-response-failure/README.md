# Job 182782/v94 retained response evidence

This is a compact local copy of the immutable remote failure evidence. The
complete remote directory remains at
`/project/tma1/ndnsf-di/evidence/spec168/tiger-large-single/`
`spec168-campaign-v3-6490395b0b976a05175f-FAILED-job-182782`.

`generation-raw.jsonl` contains one authenticated 64-token response with one
wire Request and zero token Requests. The runtime resolved the large response,
but the strict single-stage reference check classified it as
`TOKEN_MISMATCH` (`exactReferenceMatch=false`), so the campaign status is
`FAILED`. `user.log` and the manifests are retained for replay of the analyzer
boundary; no replacement identity is created.
