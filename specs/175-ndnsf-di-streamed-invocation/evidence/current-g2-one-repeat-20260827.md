# Spec175 current-source G2 one-repeat checkpoint

Date: 2026-08-27  
Subject: current dirty worktree diagnostic seal, current `build/integration-tests`  
Source seal: `/tmp/spec175-source-seal-current-OYPYod.json`  
Source-seal SHA-256: `sha256:d345544813252b6e6057d3d41194fae7a9d4badce28cebe30b3a2c24be0ed2b7`  
Manifest: `/tmp/spec175-g2-current-7tKv8a/manifest.json`  
Manifest SHA-256: `sha256:7e453119b8d676e07f08432ccbb30a2f2794271c7315f002f748ac6391ced19b`

The current-source G2 runner executed all registered I01--I20 cases once:
15 native cases and 5 Python conversation cases. The manifest contains 20/20
results, no missing cases, and zero failures. This is a diagnostic checkpoint
because the source seal records 168 dirty paths; it is not a T020 promotion
seal and does not authorize SIF, Tiger, CUDA, or performance qualification.

The separate real MiniNDN M11--M14 runs use the same seed (`1750001`) and
current source. Their machine-readable result hashes and bounded metrics are
recorded in `current-conversation-reruns-20260827.md`.
