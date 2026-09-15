# Spec186 r61 host native build receipt

This receipt records a fresh host build from source commit
`81096f3eed95361b86ccf9f063ef31ce84de076f` after the Rust toolchain preflight
repair. It is host-native evidence only; it is not a SIF, MiniNDN, CUDA or
Tiger qualification result.

| Check | Result |
| --- | --- |
| Waf output | `build-spec186-r61`; selected six runtime targets; `-j4` |
| Waf build | `197/197`, `5m31.075s`, exit 0 |
| Python binding | `_ndnsf.cpython-38-x86_64-linux-gnu.so` built by `spec180_native_build.py` |
| identity verify | `SPEC180_NATIVE_IDENTITY_OK build-spec186-r61/spec180-native-build.json` |
| import/RPATH/loader guard | `spec180_native_build.py verify`, exit 0 |
| provider/requester/authority help | exit 0 with usage text |
| ONNX assembly worker | rejects `--help` with its expected invalid-mode exit 2 |
| bundle | `.codex-tmp/spec186-app-bundle-r61`, 9 files, 215,077,410 bytes |
| bundle digest | `sha256:4a13a2fbc40d5825c94ecef71bd4522321663567da2fc0c1cd339d8f9851de3c` |

The bundle manifest is self-consistent: `runtime.tree_digest()` recomputes the
same digest. The current YOLO launcher hash is
`5508b95aceb403b1cc1f4c69352c670227baab3d47af49dbbb9d439faa167a3d`; the
current Qwen launcher hash remains
`2aa563c5aea765ca7221609233afaab13b0631b45d7f0714b6df871f004c4abf`.

The bundle manifest remains bound to the build source commit above. The later
documentation-only checkpoint `54b54bd0abf8490a7d2320b32c6e5a90f7698e1c` has a
new source seal, so this r61 binary bundle is deliberately not promoted as the
current Tiger application bundle until rebuilt against that seal.

The host bundle is also deliberately not promoted as the Tiger application bundle.
Its ELF RUNPATH still contains host-only prefixes such as
`/tmp/spec186-nacabe-install-r5/lib`, `/home/tianxing/NDN/ndn-svs/build`, and
`build-spec186-r61`. The container definition must rebuild the changing app
inside the matching SDK and verify the final `/opt/ndnsf-di/current` closure;
copying these host binaries into a SIF would recreate the hidden host-library
dependency that this task is intended to prevent.
