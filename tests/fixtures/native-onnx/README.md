Frozen tiny CPU backend inputs authored by the independent ONNX helper in
`tests/fixtures/spec170/generate_cpu_onnx_fixture.py`. Hex preserves portable
tracked bytes; the native fixture materializes them in its private directory
and removes them after the runner is released. No Python, ONNX package, model
cache, download or environment-provided model is needed at test time.

`linear`, `linear-multi`, `slice` and `unsplit` add explicit constants to their
float32 inputs; the native tests retain independent expected numeric values
and the collective comparison. `typed` preserves dynamic int64, bool and
float16 tensors through three Identity nodes.
