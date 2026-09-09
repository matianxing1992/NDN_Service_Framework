# R10-B25 `DI_NativeRequester` Build and CLI Boundary

**Status**: DONE for the bounded executable and CLI boundary; real requester/provider execution and T016 remain PARTIAL/UNQUALIFIED  
**Date**: 2026-09-09  
**Baseline**: `4187f114`  
**Owner**: `examples/DI_NativeRequester.cpp` and its registered Waf target

## Scope and stable exit

R10-B25 builds the already registered `DI_NativeRequester` target and observes only its
command-level contract. The stable exit is a successfully linked executable whose help,
argument-usage and malformed-schema paths are independently observable and fail closed. No
model, Core, Provider, network, cross-process or qualification claim is made by this batch.

## Minimum Review Record

Review path: `/home/tianxing/.codex/skills/review-agent/SKILL.md`  
Review SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`  
Review baseline: `4187f114`; no product source diff was introduced in this batch.

The five review lanes were checked against the existing source, Waf registration, target and
CLI oracle:

| Lane | Status | Files / symbols and boundary |
| --- | --- | --- |
| `production entry/callers` | covered | `examples/DI_NativeRequester.cpp::main`; Waf target `DI_NativeRequester` |
| `implementation and wire` | covered | argument parser, `ndnsf-di-native-requester-v1` schema guard, native runtime composition |
| `test/harness/oracle` | covered | `--help`, bare invocation usage, invalid-schema rejection; output-file absence oracle |
| `build/source closure` | covered | `examples/wscript` source registration, linked executable and `ldd` closure |
| `migration/evidence` | gap | real config/model keys, Core/Provider transport, maintained callers, no-Python and T016 remain open |

The first static command initially used an incorrect literal expectation for help text and failed
its own assertion. The corrected check tests the actual `std::string(argv[1]) == "--help"` branch
and the usage text; no source defect was found. This is recorded as a static-check miss, not a
runtime or product failure.

## Validation record

All commands used the system-first path `PATH=/usr/bin:/bin:/usr/sbin:/sbin` where a build or
runtime command required it. Because R10-B24 recorded swap-in pressure, the native build used the
documented `-j2` fallback and did not run concurrently with another Waf build.

| Command / observation | Result |
| --- | --- |
| corrected source/Waf static checks and `git diff --check` | PASS; `main`, help branch, usage, schema guard, target and source registration all present |
| `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` before build | `ok: true`; 17 tasks, 40 execution cards, 951 local links; runtime tests remain `NOT_RUN` |
| `./waf -o build-nac182 build --targets=DI_NativeRequester -j2` | exit `0`; Waf `build finished successfully (1m17.740s)`; actual target `.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester` |
| `DI_NativeRequester --help` | exit `0`; usage and `ndnsf-di-native-requester-v1` schema text observed |
| bare `DI_NativeRequester` | exit `2`; usage emitted on stderr |
| invalid config `{"schema":"bad"}` with empty input | exit `1`; `NATIVE_REQUESTER_FAILED: unsupported requester configuration`; no output file created |
| `ldd` on target | PASS; all listed libraries resolved, no `not found` entry |
| `vmstat 1 2` after build | second sample `si=372`, `so=0`; no sustained swap-out observed, so the next native build remains conservatively `-j2` unless a fresh resource check justifies change |

Raw command outputs, return codes, target metadata, `ldd`, `file` and `vmstat` are preserved in
`.codex-tmp/spec182-r10-b25-native-requester-cli-20260909-run1/`.

## Batch Retrospective

- `static`: corrected self-check passed; the initial expected-string typo was a static review miss.
- `compile/link`: target rebuilt and linked successfully with the documented system-first toolchain
  and `-j2`; no unresolved ELF dependency was observed.
- `runtime/test`: command-level CLI checks passed for help, usage rejection and schema rejection.
- `unobserved`: real requester configuration, model assembly, grant/admission exchange, Core and
  Provider execution, cross-process transport, maintained callers, I02--I08, legacy retirement,
  no-Python behavior and T016 qualification.

## Closure decision

`CLOSED_FOR_VALIDATION` for the executable/CLI boundary with
`STATIC_PASS`, `BUILD_PASS` and focused CLI behavior evidence. CLI smoke proves wiring and
fail-closed parsing only; it is not native request/result, parity or qualification evidence.
The next production boundary is a real requester/provider execution with a declared fixture and
independent native result oracle.
