# Contract: Two-Provider Placement and Execution

Selection MUST bind the prepared manifest to two providers:

```json
{
  "attemptId": "...",
  "manifestDigest": "sha256:...",
  "protectionEpoch": "...",
  "placements": [
    {"providerId": "provider-0", "stageIndex": 0, "start": 0, "endExclusive": 14, "layerDigest": "sha256:..."},
    {"providerId": "provider-1", "stageIndex": 1, "start": 14, "endExclusive": 28, "layerDigest": "sha256:..."}
  ]
}
```

Before Selection, providers may inspect offers and manifest summaries only. After Selection, each provider verifies the signed attempt, manifest digest, epoch, role, range and layer digest before reading its package. It MUST NOT read the other provider's package or create a runner for an unselected placement.

The execution oracle must observe a real production handoff between the two stages. If the current Core wire lacks the required hidden-state contract, the run is `PROTOCOL_BOUNDARY`; an in-process Python or direct ORT shortcut is invalid.
