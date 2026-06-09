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

The business-logic API now covers ordinary service invocation, provider ACK
decisions, application-defined ACK selection, asynchronous requests,
provider-side collaboration handlers, user-side collaboration requests, and the
generic NDNSF large-data reference helpers used by higher-level applications.

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

Applications that need service-specific provider selection can inspect the ACK
candidates collected during the ACK window:

```python
response = user.request_service_select(
    "/Storage/Insert",
    payload,
    selector=lambda candidates: [
        item.provider_name for item in candidates if item.status
    ][:2],
    ack_timeout_ms=300,
    timeout_ms=5000)
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

## Collaboration and Large Data

The Python wrapper exposes the current generic collaboration path. Providers
can register collaboration handlers with allowed roles, and users can request a
multi-provider collaboration by describing roles, key scopes, dependencies, and
optional artifact Data names:

```python
from ndnsf import CollaborationRole, ServiceProvider, ServiceUser

provider = ServiceProvider()

@provider.collaboration_handler(
    "/AI/DistributedInference",
    allowed_roles=["/Stage/0", "/Stage/1"])
def handle_stage(ctx, payload: bytes) -> None:
    ctx.publish("activation", "/stage/output", payload)
    ctx.publish_final_response(b"ok")

provider.run()

user = ServiceUser()
response = user.request_collaboration(
    "/AI/DistributedInference",
    b"input",
    roles=[CollaborationRole(role="/Stage/0"), CollaborationRole(role="/Stage/1")],
    key_scopes={"activation": ["/Stage/0", "/Stage/1"]},
    dependencies=[])
```

For objects that exceed the inline/single-segment threshold, Python
applications should use the same NDNSF large-data rule as C++ applications:
publish hybrid AES-GCM encrypted, signed segmented Data through the Core helper
and carry only a compact reference in the service message. The wrapper provides
`LargeDataReference`, `encode_large_data_reference_payload(...)`, and
`parse_large_data_reference_payload(...)` for that reference payload.
Collaboration handlers also provide `publish_large(...)`,
`publish_large_reference(...)`, `fetch_large(...)`, and
`fetch_large_reference(...)` for scoped collaboration data.

For NDN-native segmented objects, the wrapper exposes helpers around ndn-cxx
segment production and fetching:

```python
from ndnsf import (
    SegmentedObjectProducer,
    StoredDataProducer,
    fetch_segmented_object,
    fetch_segmented_data_packets,
    make_segmented_data_packets,
)

producer = SegmentedObjectProducer("/example/object", b"payload").start()
payload = fetch_segmented_object(producer.versioned_name)
producer.stop()

packets = make_segmented_data_packets("/example/native-packets", b"payload")
stored = StoredDataProducer("/example/native-packets", [p.wire for p in packets]).start()
fetched_packets = fetch_segmented_data_packets("/example/native-packets")
stored.stop()
```

`SegmentedObjectProducer` creates signed segmented Data from a payload.
`StoredDataProducer` serves already-signed Data wire packets without rewriting
them. The Core encrypted large-data helpers add the hybrid encryption layer
when authorization-protected application data is required. Higher-level
packages such as `NDNSF-DistributedInference` decide whether
those objects are model artifacts, runner bundles, activations, or other
application data.

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

The current Python binding covers ordinary service request/response handlers,
provider ACK decisions, custom ACK selection, asynchronous requests,
collaboration handlers, collaboration requests, encrypted large-data publishing,
large-data references, and NDN segmented Data helpers. Higher-level examples
may still use `NDNSFSession` when they need to launch existing C++ binaries or
coordinate multi-process experiments.
