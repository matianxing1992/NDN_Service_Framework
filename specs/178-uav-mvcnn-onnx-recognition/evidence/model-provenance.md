# T001 model provenance

The qualified subject is the locally authored MVCNN-family checkpoint in
`NDNSF-UAV-APP/tools/mvcnn_model.py`, licensed MIT. The source digest is
`sha256:4a8610c47ed89159293cf5b36939360ca257b9cf391b0691add652d45ce05a76`.

The deterministic native qualification command was:

```text
python3 NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py --output-dir NDNSF-UAV-APP/models
```

It produced 48 six-view samples (three classes plus the checked-in functional
car fixture) and native CPU accuracy `1.0` on that qualification subject. The
result is an export/provenance gate only; the generated car fixture remains
excluded from UAV-domain accuracy claims.

| item | digest / value |
| --- | --- |
| checkpoint | `sha256:6acdbf2c34c7a2cd2cc6f7c76d3ced2fcd4e99f71e3907131dbbc113bb25b279` |
| checkpoint path | `NDNSF-UAV-APP/models/mvcnn_vehicle_cpu.pt` |
| source revision | `local-source:sha256:4a8610c47ed89159293cf5b36939360ca257b9cf391b0691add652d45ce05a76` |
| class map | `car`, `truck`, `person` |
| native runtime | PyTorch `2.4.1+cpu`, Python `3.8.10` |
| native sample count | `48` six-view samples |
| native qualification | `1.0` (qualification-only) |

