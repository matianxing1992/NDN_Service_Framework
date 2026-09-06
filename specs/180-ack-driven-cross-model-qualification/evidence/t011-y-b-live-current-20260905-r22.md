# T011-B current-source Y-B live evidence — 2026-09-05 r22

Status: focused wiring evidence only. This record does not close T011, T014,
or T015 and is not SIF/Tiger qualification evidence.

## Subject

- Case: `Y-B`
- Run directory: `.codex-tmp/spec180-yolo-y-b-current-source-output-20260905-r22`
- Request ID: `/spec180-y-b-0bdb59a0b2dd0b51`
- Attempt: `attempt-1`
- Terminal record: `YOLO_ACK_DRIVEN_RESULT status=true`
- Subcase record: `status=PASS`, `outcome=CONTROL`,
  `reason=TERMINAL_RESPONSE_VERIFIED`, `childCount=7`,
  `terminalResponse=true`, boundary `TERMINAL_RESPONSE`

## Runtime publication and identity binding

The run's `runtime-publication.json` and
`runtime-publication-receipt.json` agree on the signed catalogue and all four
role artifact records:

- Catalogue: `/example/controller/NDNSF/DI/catalogue/v1`
- Catalogue signer: `/example/controller`
- Catalogue payload digest:
  `sha256:8ccf140683425f2c8ad34d52ba98664fafaadf7a9891415ecd5352d578ab3d21`
- Candidate/package manifest digest:
  `sha256:9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d`
- Shared candidate artifact prefix:
  `/example/controller/NDNSF/DI/ARTIFACT/6b6d318f6e9b489c38871e57952252fcae2ff7439db5e3f8776774c534caa52b`

Role artifact payload digests are:

| Role | Payload digest |
|---|---|
| `BackboneNeck` | `sha256:933b71f2d464b0451af585efd2611bded6f5fa6e845a3bbe8012483186df8393` |
| `DetectShard0` | `sha256:8a0e57dcbb6e3b77bd55b58a5084ec3c53878615a014625a4ae880e1edfe9037` |
| `DetectShard1` | `sha256:5f1417d26c0ed60cc01f2f9930a9920ee72716779f546c87d6b392ae2ecd80b9` |
| `Merge` | `sha256:f5b5c9733c740c77f4d0a7464912ebfc7314d49daf70b184b3f74aaabb3f209d` |

## Live exchange and numerical oracle

All four Provider children reached the terminal exchange. The exact native
publication counts observed in the role logs were:

| Role | `NDNSF_DI_EXACT_DATA_PUBLISHED` count |
|---|---:|
| `BackboneNeck` | 381 |
| `DetectShard0` | 286 |
| `DetectShard1` | 94 |
| `Merge` | 0 |

The terminal numerical record is `matched=true` against the fixed oracle:

- Input tensor digest:
  `sha256:5a56c22102ec1caa3409b30a10a33d586cbd7b305c90dcd3b99b39b44598ca7f`
- Fixture digest:
  `sha256:7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c`
- Oracle digest:
  `sha256:ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175`
- Plan digest:
  `sha256:784c134fc880347f3b800fa039851fb9a52e2e869b43aa81c539221029ac46b4`
- Response digest:
  `sha256:4051addc95ad48e5865bc91c2d29d9fe307dad55bb90740020848aede16d649e`
- Observed shape / expected shape: `[1,50,6]` / `[1,50,6]`
- Maximum absolute error: `0.0005340576171875`
- Tolerances: `atol=0.001`, `rtol=0.0001`

The run returned `runner_exit=0`. No `DI_INPUT_FETCH`, `DI_*FAIL`,
`YOLO_MERGE_INSUFFICIENT`, or native Merge-assignment diagnostic was observed
in the four Provider logs. The seven supervised children were cleaned by the
driver after the terminal result.

## Boundary

This is current-source, real-NFD local evidence for the shared Y-B path after
the native compact transport, multi-tensor scope, CPU-device alias, and Merge
maximum-row repairs. It remains pre-T014 evidence: the source tree still needs
the Spec180 source-binding closure, the fresh design-code convergence audit,
and the registered T015 inventory before any candidate can be sealed or any
SIF/Tiger run can start.
