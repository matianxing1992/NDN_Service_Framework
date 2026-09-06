# T038 Native assembly helper checkpoint — 2026-09-01

## Defect found and repaired

The production C++ bridge emits the sealed role and recipe request in
snake-case JSON, while the Python helper previously passed the request directly
to `RoleAssemblySpec` and `CertifiedOnnxAssemblyRecipe`.  The recipe dataclass
therefore rejected the live request with:

```text
TypeError: __init__() got an unexpected keyword argument 'adapterDescriptorDigest'
```

The helper now performs an allowlisted, conflict-checked normalization of the
C++ wire spelling and the Python dataclass spelling.  Nested resource-envelope
fields are normalized separately; unknown fields and alias conflicts are
rejected before model assembly.

The same boundary also normalizes concrete tensor dimensions represented as
JSON strings by the C++ projection (for example, `"1"`) before comparing them
with ONNX Runtime's integer dimensions.  Symbolic dimensions remain strings.
Without this normalization, a correctly certified native role could be
rejected after the recipe had already passed digest validation.

## Focused evidence

```text
pytest -q tests/python/test_spec175_native_assembly_helper.py
2 passed

pytest -q tests/python/test_spec175_native_assembly_helper.py \
  tests/python/test_spec175_contract_gate.py
17 passed
```

The helper regression uses the exact snake-case shape emitted by
`NativeCanonicalOnnxAssembler::assemblyRequestJson`, runs four independent
Provider identities (the 1/2/4-role coverage set), loads the assembled ONNX
bytes through CPU ONNX Runtime, and rejects a mutated graph digest without
creating output files.

The native fixture `Spec175NativeAssembly/AssignmentBoundRootSourceAndCachePath`
then exercised the C++ bridge itself. It fetched an assignment-bound canonical
root, resolved its separately named source through the encrypted-large-data
port, invoked the helper, signed the manifest, reused the content-addressed
cache on an identical call, and rejected a source mutation. That run exposed
two bridge defects which are now fixed: `assemblyRequestJson` emitted one extra
closing brace, and `ndn-cxx`'s uppercase SHA-256 text did not satisfy the
cross-language lowercase digest grammar. The integration target now links the
assembler source and the fixture passes after a clean rebuild.

The same native test now exercises the fail-closed mutation set: missing signer,
missing assignment-bound root, graph-digest mutation, initializer-digest
mutation, recipe-digest mutation, source corruption, and a content-addressed
cache conflict. The direct `CollaborationContext` case first completes the
required `fetchArtifact()` prefetch and then proves that the production
overload reads the ACTIVE root from the context before attempting source fetch.
Both `Spec175NativeAssembly` cases pass after the clean integration rebuild.

## Qualification boundary

This is a focused implementation closure, not MiniNDN qualification. The
registered native assembly cases now run as three separate processes through
the Python oracle:

```text
pytest -q tests/python/test_spec175_native_assembly.py → 3 passed
```

Each case starts with an empty Provider-local cache, fetches the assignment
root and separately named source, invokes the real helper, signs and activates
the content-addressed result, and constructs the C++ `OnnxRuntimeModelRunner`
to perform the real CPU ORT load/warmup. The cases cover one, two, and four
independent Provider identities. The source-contract regression also mutates
the launcher rejection markers and fails closed when the formal serving path
would accept `--artifact-references` or a ready-made role file:

```text
pytest -q tests/python/test_spec175_contract_gate.py → 18 passed
```

T038 is therefore complete at its implementation boundary. MiniNDN remains a
later T022 qualification gate and is not required to close this task.
