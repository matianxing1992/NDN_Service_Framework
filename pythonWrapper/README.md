# NDNSF Python Wrapper

This directory contains an installable Python wrapper around the NDNSF C++
runtime and selected C++ applications. It intentionally does not change the
NDNSF core logic.

The wrapper provides two layers:

- `ServiceController` / `ServiceProvider` / `ServiceUser`: the preferred
  Python business-logic API.
  Python defines handlers and request payloads, while the `ndnsf._ndnsf`
  pybind11 extension calls the NDNSF C++ runtime for SVS, NAC-ABE, token
  verification, and Face operations.
- `NDNSFSession`: lower-level process orchestration for launching existing C++
  applications in experiments.

Application-specific orchestration, such as which AI roles to start or which
policy file to use, belongs in user code or `examples/`, not in this wrapper
package.

AI/model-specific APIs live above this wrapper in
`../NDNSF-DistributedInference/`. That package uses `ServiceUser`,
`ServiceProvider`, encrypted artifacts, and collaboration primitives from this
wrapper, but exposes model-plan and inference-oriented APIs to application
developers.

## Install

Install the Python package from the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
```

For `NDNSFSession` process orchestration, NDNSF C++ binaries must also be
installed or discoverable. The wrapper searches for binaries in this order:

1. an absolute/relative path passed as `binary`;
2. `binary_dir=...` in `NDNSFSession`;
3. the `NDNSF_BINARY_DIR` environment variable;
4. the process `$PATH`;
5. `repo_root/build/examples` only when `repo_root` is explicitly passed.

When running against an uninstalled source-tree build, use:

```bash
export NDNSF_BINARY_DIR=/path/to/ndn-service-framework/build/examples
export NDNSF_LIBRARY_DIR=/path/to/ndn-service-framework/build
```

`ServiceProvider` and `ServiceUser` use the compiled Python extension instead
of launching host binaries. During source-tree development, rebuild it with:

```bash
cd pythonWrapper
python3 setup.py build_ext --inplace
```

## Reusable API

The preferred API keeps application logic in Python:

```python
from ndnsf import ServiceProvider

provider = ServiceProvider()

@provider.handler("/HELLO")
def hello(request: bytes) -> bytes:
    return b"HELLO"

provider.run()
```

```python
from ndnsf import ServiceController

controller = ServiceController(
    policy_file="examples/hello.policies",
    bootstrap_identities=[
        "/example/hello/provider",
        "/example/hello/user",
    ])
controller.run()
```

```python
from ndnsf import ServiceUser

user = ServiceUser()
for service in user.get_allowed_services():
    print(service.service, service.provider_service)

response = user.request_service(
    "/HELLO",
    b"HELLO",
    ack_timeout_ms=300,
    timeout_ms=5000)

print(response.status, response.payload)
```

For open-loop applications or benchmarks, use the asynchronous API:

```python
user.start()
user.request_service_async(
    "/HELLO",
    b"HELLO",
    on_response=lambda response: print(response.status, response.payload),
    on_timeout=lambda request_id: print("timeout", request_id),
    ack_timeout_ms=300,
    timeout_ms=5000)
user.stop()
```

`ServiceProvider` exposes service-independent runtime options such as
`group`, `controller`, `provider_prefix`, `trust_schema`, `handler_threads`,
and `serve_certificates`. `ServiceUser` exposes `group`, `controller`, `user`,
`trust_schema`, `permission_wait_ms`, `handler_threads`, `ack_threads`,
`adaptive_admission`, and `serve_certificates`; each `request_service()` call
also accepts `ack_timeout_ms`, `timeout_ms`, and `strategy`
(`"first-responding"`, `"random-selection"`, or `"all-selected"`).
After permission discovery, `get_allowed_services()` returns the services the
controller authorized for the user. Each entry contains the unified service
name used by `request_service()` and the full provider/service permission name.

For experiment orchestration, use `NDNSFSession`:

```python
from ndnsf import NDNSFSession, ControllerConfig, ProviderConfig, UserConfig

providers = [
    ProviderConfig(
        name="provider-a",
        binary="MyProviderApp",
        args=("--service", "/Example/Service"),
        service="/Example/Service",
    )
]

with NDNSFSession(
    binary_dir="/usr/local/bin",
    log_dir="results/my_python_app") as ndnsf:
    ndnsf.configure_svs_group("/example/group")
    ndnsf.start_controller(
        ControllerConfig(policy_file="examples/my_app.policies"))
    for provider in providers:
        ndnsf.start_provider(provider)
    result = ndnsf.run_user(
        UserConfig(
            name="user",
            binary="MyUserApp",
            args=("--service", "/Example/Service")),
        timeout=20)
```

Both layers keep core NDNSF mechanisms in C++. Python code supplies application
payloads, handlers, and orchestration policy.

## Layout

- `ndnsf/service.py`: Python business-logic API.
- `src/ndnsf/_ndnsf.cpp`: pybind11 extension backing the business API.
- `ndnsf/api.py`: reusable NDNSF session/configuration API.
- `ndnsf/runtime.py`: low-level process management for NDNSF binaries.
- `../examples/python/`: application-specific Python examples built on top of
  the generic API.
- `../NDNSF-DistributedInference/`: optional high-level distributed inference
  package built on top of this generic wrapper.

## Source-Tree Development

For development without installing C++ binaries system-wide, build the examples
and point the wrapper at the build output:

```bash
./waf configure
./waf build
export NDNSF_BINARY_DIR=$PWD/build/examples
export NDNSF_LIBRARY_DIR=$PWD/build
```

## Dry Run

```bash
python3 examples/python/hello_service.py --dry-run
python3 examples/python/ai_distributed_collaboration.py --dry-run
```

## Run Locally

The local examples can start a local NFD for a single-host smoke test. If an
NFD is already running, it is left untouched; the helper only stops an NFD that
it started itself:

```bash
python3 examples/python/hello_service.py --run --start-local-nfd
python3 examples/python/ai_distributed_collaboration.py --run --start-local-nfd
```

The current Python binding covers ordinary service request / response handlers
and provider ACK decisions. Distributed collaboration examples still use the
lower-level orchestration layer until collaboration-specific Python bindings
are added.
