# Spec186 base SIF digest drift — r76

The temporary base input `.codex-tmp/spec186-repaired-base-final.sif` was
re-read before r76 handoff rendering. It remained 3,088,814,080 bytes with
the recorded 2026-09-13 timestamp, but its stable SHA-256 was:

```text
sha256:d9be92ea6dd74521e43a0751d6d16651ffc2320541aa97207ec1db51261446bd
```

The preceding r74/r75 lock expected:

```text
sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c
```

`prepare-development-handoff.py render` rejected the stale identity with
`HANDOFF_BASE_DIGEST`. This receipt is a boundary failure, not a SIF build or
runtime result. A new lock must bind the observed digest before another local
build is allowed.
