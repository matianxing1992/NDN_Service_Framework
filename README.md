# ndn_service_framework

NDNSF is a generic dynamic service framework over Named Data Networking. This
repository contains the C++ core runtime, Python bindings, a distributed repo
prototype, a distributed inference package, and a UAV application used as an
application-driven validation workload. New applications should use unified
service names and the dynamic user/provider/controller APIs. The old generated
service/stub path is no longer the supported development direction.

Current major components:

```text
ndn-service-framework/        C++ core runtime and generic dynamic API
pythonWrapper/                Python business-logic API and process orchestration
NDNSF-DistributedRepo/        Distributed repo prototype and Python binding
NDNSF-DistributedInference/   Higher-level distributed inference package
NDNSF-UAV-APP/                UAV network application over NDNSF
examples/                     C++ and Python smoke/regression examples
Experiments/                  MiniNDN and experiment harnesses
RELEASE/                      Local release packaging artifacts and manuals
```

The framework contribution is the service runtime itself: provider discovery,
permission distribution, NAC-ABE-backed message protection, one-time token
handshakes, ACK/Selection/Response execution, Targeted invocation for known
providers, trusted local invocation inside one process, ServiceContainer-based
process composition, and a common large-data reference abstraction. The UAV and
DistributedInference applications are application layers that validate and
stress these framework mechanisms.

## 1. Prerequisites

To keep the stack version-consistent, use the following repositories:

```text
ndn-cxx: https://github.com/matianxing1992/ndn-cxx
NDNSD:   https://github.com/matianxing1992/NDNSD
ndn-svs: https://github.com/matianxing1992/ndn-svs
NAC-ABE: https://github.com/matianxing1992/NAC-ABE
NDNSF:   https://github.com/matianxing1992/NDN_Service_Framework
```

The recommended installer below checks whether the external NDN dependencies
are already installed. If a dependency is missing, it clones the corresponding
repository from the list above, builds it, and installs it before building
NDNSF.

## 2. Installation

The recommended installer is the top-level stack script:

```bash
sudo ./install_ndnsf_stack.sh
```

It installs the stack in dependency order:

1. Install default build/runtime OS packages needed by the NDNSF stack.
2. Check external dependencies with `pkg-config`.
3. Clone, build, and install missing dependencies:
   `ndn-cxx`, `NDNSD`, `ndn-svs`, `OpenABE`, and `NAC-ABE`.
4. Build the NDNSF C++ core and bundled C++ subprojects with `waf`.
5. Install the NDNSF Python wrapper package, `ndnsf`.
6. Install the NDNSF-DistributedRepo Python binding, `py_repoclient`.
7. Install the NDNSF-DistributedInference Python package.
8. Run a small Python import/API smoke test.

The default OS package set is intended to build and run:

```text
ndn-cxx, NDNSD, ndn-svs, OpenABE, NAC-ABE, and NDNSF
```

Additional optional package groups can be enabled when needed:

```bash
# Extra packages commonly used by tests and documentation.
sudo ./install_ndnsf_stack.sh --with-system-tests-deps

# Extra packages commonly used by MiniNDN/Mininet experiments.
sudo ./install_ndnsf_stack.sh --with-minindn-deps

# Extra packages commonly used when building NFD/NLSR from source.
sudo ./install_ndnsf_stack.sh --with-nfd-nlsr-deps
```

`apt-get install` is idempotent, so already installed packages are skipped by
the package manager.

By default, dependency source trees are reused or cloned under `dependencies/` next to `install_ndnsf_stack.sh`. The directory is created automatically if it does not
exist. Use `--deps-dir` to choose a different source directory:

```bash
sudo ./install_ndnsf_stack.sh --deps-dir ./dependencies
```

If the dependencies are already installed and you only want to rebuild NDNSF:

```bash
./install_ndnsf_stack.sh --no-dependencies --no-system-install
```

To force rebuilding all external dependencies from the local source trees or
from freshly cloned repositories:

```bash
sudo ./install_ndnsf_stack.sh --force-dependencies
```

### OpenABE and OpenSSL note

NAC-ABE depends on OpenABE. The upstream OpenABE code is sensitive to OpenSSL
versions and is known to work most reliably with OpenSSL 1.1.x. Ubuntu 20.04
ships OpenSSL 1.1, but Ubuntu 22.04 and 24.04 ship OpenSSL 3 by default. To
avoid replacing the system OpenSSL, the installer builds OpenABE with its
private OpenSSL 1.1 dependency when `libopenabe` is missing. The private OpenABE
installation is placed under:

```text
dependencies/local/openabe
```

This keeps OpenABE/NAC-ABE compatible on newer Ubuntu releases without changing
the OpenSSL used by system tools such as `apt`, `git`, `curl`, or Python.

For source-tree development, or when you do not want to install C++ libraries
and headers system-wide, use:

```bash
./install_ndnsf_stack.sh --no-system-install
```

Useful variants:

```bash
./install_ndnsf_stack.sh --configure --with-examples
./install_ndnsf_stack.sh --configure --with-tests
./install_ndnsf_stack.sh --no-system-install --with-examples
./install_ndnsf_stack.sh --no-dependencies --no-system-install
```

Repository URLs can be overridden if needed:

```bash
NDNCXX_REPO_URL=https://github.com/matianxing1992/ndn-cxx \
NDNSD_REPO_URL=https://github.com/matianxing1992/NDNSD \
NDNSVS_REPO_URL=https://github.com/matianxing1992/ndn-svs \
OPENABE_REPO_URL=https://github.com/zeutro/openabe \
NACABE_REPO_URL=https://github.com/matianxing1992/NAC-ABE \
sudo ./install_ndnsf_stack.sh --force-dependencies
```

Manual C++-only installation is still possible:

```bash
./waf configure
./waf
sudo ./waf install
```

If you install manually and also need Python APIs, install the Python packages
after the C++ build:

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedRepo/pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

### 2.1 MiniNDN quick checks

After an API or script update, run the short MiniNDN/script health suite:

```bash
python3 Experiments/NDNSF_Run_Minindn_Quick_Checks.py
```

The default suite first runs syntax/import sanity for the updated NDNSF, Repo,
DI, and UAV MiniNDN experiment scripts, then calls the no-MiniNDN quick-smoke
branches for the longer legacy experiment launchers. It then covers the Python
HELLO MiniNDN smoke, DistributedRepo single-object MiniNDN quick smoke, DI local
YOLO layout smoke, and UAV launcher quick smoke. It intentionally skips the
slower DI native-provider MiniNDN smoke unless requested:

```bash
python3 Experiments/NDNSF_Run_Minindn_Quick_Checks.py --include-di-minindn
```

## 3. How-to

### 3.1 Generic dynamic API, preferred for new applications

New applications should use the framework-core generic dynamic API directly.

Provider side:

```cpp
ndn_service_framework::ServiceProvider provider(
  face,
  ndn::Name("/muas/group"),
  providerCert,
  aaCert,
  "examples/trust-any.conf");

provider.addHandler<ObjectDetectionRequest, ObjectDetectionResponse>(
  ndn::Name("/ObjectDetection/YOLOv8"),
  [](const ndn::Name& requesterIdentity,
     const ObjectDetectionRequest& request,
     ObjectDetectionResponse& response) {
    // Service logic starts here.
    response.set_label("person");
  });
```

User side:

```cpp
ndn_service_framework::ServiceUser user(
  face,
  ndn::Name("/muas/group"),
  userCert,
  aaCert,
  "examples/trust-any.conf");

ObjectDetectionRequest request;
request.set_image("frame-bytes");

user.RequestService<ObjectDetectionRequest, ObjectDetectionResponse>(
  ndn::Name("/ObjectDetection/YOLOv8"),
  request,
  300, // ACK collection window in milliseconds.
  ndn_service_framework::strategy::FirstResponding,
  1000, // Overall response timeout in milliseconds.
  [](const ObjectDetectionResponse& response) {
    // Handle typed response.
  },
  [](const ndn::Name& requestId) {
    // Handle timeout.
  });
```

For new C++ application code, keep the public API surface small:

```text
Provider normal service:       addHandler<RequestT, ResponseT>(serviceName, handler)
Provider known-target service: addTargetedService(serviceName, handler)
User normal service:           RequestService<RequestT, ResponseT>(serviceName, request, ackMs, policy, timeoutMs, onResponse, onTimeout)
User known-target service:     RequestServiceTargeted<RequestT, ResponseT>(provider, serviceName, request, onResponse, onTimeout, timeoutMs)
Same-process helper:           ServiceContainer::addLocalService<RequestT, ResponseT>(serviceName, handler)
```

Lower-level overloads that accept raw `RequestMessage`, legacy integer strategy
values, or explicit provider lists remain available for framework internals,
tests, and compatibility. They are not the recommended starting point for new
applications.

For known-provider low-latency commands, such as UAV flight-control/MAVLink
execution, use targeted invocation. Targeted invocation still uses NDNSF
`RequestMessage`/`ResponseMessage`, signing, permission checks, one-time token
checks, and replay protection. It skips the normal ACK/Selection phase only
after a token batch has been bootstrapped for that provider/service:

```cpp
provider.addTargetedService(
  ndn::Name("/UAV/MAVLink/Execute"),
  handler);

user.RequestServiceTargeted<MavlinkCommand, MavlinkResult>(
  ndn::Name("/example/uav/drone/A"),
  ndn::Name("/UAV/MAVLink/Execute"),
  command,
  onResponse,
  onTimeout,
  timeoutMs);
```

Known-provider low-latency calls use only the `Targeted` API names. The old
Direct API names are intentionally not kept as compatibility aliases.

Security model for Targeted invocation:

```text
First call or token refill:
  TargetedBootstrapRequest -> ACK -> SELECTION -> RESPONSE
  The provider response includes a batch of future one-time token pairs.

Fast path while cached token pairs remain:
  REQUEST -> RESPONSE
  The request carries one unused ProviderToken.
  The response echoes the paired UserToken.
  The provider consumes the ProviderToken before executing the handler.
```

This keeps known-provider commands low-latency without losing provider
authorization or replay resistance. When the cached token pool is exhausted,
the next `RequestServiceTargeted(...)` call automatically uses the bootstrap
flow again.

For payloads that exceed the inline/single-segment threshold, NDNSF uses one
common large-data reference abstraction. Small request and response payloads
remain inline in `RequestMessage.payload` and `ResponseMessage.payload`.
Large request inputs should be prepared with the Core large-data helper, which
publishes the bytes as signed segmented NDN Data and puts only a
`LargeDataReference` in the request payload. Large responses are handled
automatically by NDNSF Core: if a successful `ResponseMessage.payload` exceeds
the configured threshold, the provider publishes it as signed segmented NDN
Data and replaces the inline response payload with a `LargeDataReference`.
The user runtime detects that reference, fetches the segments, decrypts and
verifies the object, and then delivers the original response payload to the
same application callback. Application response APIs do not need to change.

Large request and response objects use hybrid message encryption rather than
NAC-ABE encryption over the whole large payload. The payload body is encrypted
with AES-GCM and stored as a segmented `HybridMessageEnvelope`; NAC-ABE is used
only to wrap the small message key when that key is not already cached. Large
requests use the `REQUEST-LARGE` envelope type, while automatic large responses
use `RESPONSE-LARGE` so the large-object key state cannot collide with the
outer control `REQUEST`/`RESPONSE` messages. This preserves normal
service-level authorization while avoiding the high cost of NAC-ABE
`produce/consume` on large catalog snapshots, model artifacts, activations,
recordings, and other large response bodies.

NDNSF providers also keep a short pending-Interest queue for Data served from
their in-memory storage. If an Interest for a predictable Data name arrives
before the corresponding Data has been produced, the provider retains that
Interest until its normal InterestLifetime expires; when matching Data is later
inserted, the provider replies immediately. This is a transport optimization
for large-data references, repo objects, and distributed-inference activation
objects. It does not change the Request/ACK/Selection/Response protocol, Data
names, signatures, encryption, or application callbacks.

Provider collaboration large-data fetches default to a 30-second Interest
lifetime (`NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS`). This is intentionally
longer than a normal low-latency command timeout because distributed-inference
roles may prefetch a deterministic activation name before the upstream role has
finished publishing the segments. Experiments can still lower or raise this
value explicitly with the environment variable.
Set `NDNSF_COLLAB_LARGE_FETCH_INIT_CWND` to tune the initial SegmentFetcher
pipeline window for collaboration large-data fetches; the default is `8`.
Set `NDNSF_COLLAB_LARGE_FETCH_TIMING=1` to emit Core-level SegmentFetcher
timing lines for these collaboration large-data fetches. The DI MiniNDN
regression parses those lines into `collab-large-fetch-stats.json` so the
application-level dependency wait can be compared with native segmented fetch
time.

The default automatic response threshold is 6000 bytes and can be changed or
disabled with environment variables:

Automatic response references default to a 6000-byte threshold, which keeps the
inline control response below the normal single-segment payload size. Request
helpers can choose a stricter threshold for known large inputs. The response
threshold can be adjusted or disabled with:

```bash
NDNSF_RESPONSE_LARGE_DATA_THRESHOLD=4096 ./your-app
NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1 ./your-app
```

User-side resolution of automatic large response references uses the same
segmented-fetch discipline. `NDNSF_RESPONSE_LARGE_INTEREST_LIFETIME_MS`
controls the Interest lifetime, `NDNSF_RESPONSE_LARGE_FETCH_INIT_CWND`
controls the initial SegmentFetcher window, and
`NDNSF_RESPONSE_LARGE_FETCH_TIMING=1` emits timing logs. These settings are
useful when an application, such as distributed inference, intentionally returns
large final outputs through the response-reference path.

For trusted composition inside one process, NDNSF also provides
`LocalServiceRegistry`. This is not a network invocation mode and is not
visible to remote callers. A service becomes local-callable only when the
container explicitly registers it in the local registry; otherwise local calls
fail closed. Local invocation bypasses NDNSF signing, NAC-ABE, permission
fetching, SVS publication, and token/replay checks, so it should only be used
inside one trusted process or service container:

```cpp
ndn_service_framework::LocalServiceRegistry localRegistry;

localRegistry.registerLocalService<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  telemetryHandler);

auto result = localRegistry.localInvoke<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  request);

auto future = localRegistry.localInvokeAsync<TelemetryRequest, TelemetryStatus>(
  ndn::Name("/UAV/Telemetry/GetStatus"),
  request);
```

Use normal `RequestService(...)` or `RequestServiceTargeted(...)` for any
cross-process, cross-node, or untrusted caller. Local invocation does not add
`/NDNSF/LOCAL/...` names and cannot be requested by a remote node.

For larger service-oriented applications, NDNSF core also provides
`ServiceContainer` as an in-process runtime composition and lifecycle boundary.
It does not replace `ServiceController`, `ServiceUser`, `ServiceProvider`, or
`LocalServiceRegistry`; it owns or references them so an application can manage
multiple roles under one process-level configuration. A container may hold any
combination of controller, user, provider, and local helper roles. It does not
require every process to run every role. This is useful for applications such as
UAV-APP and DistributedInference, where one process may act as a user, a
provider, a local helper host, and sometimes an embedded controller runtime at
the same time.

`ServiceContainer` is responsible for:

```text
managing multiple ServiceController, ServiceUser, ServiceProvider, helper, and
local-only modules inside one process;
providing a shared process-level runtime configuration;
coordinating lifecycle start/stop hooks for application-owned modules;
exposing LocalServiceRegistry for trusted same-process composition;
keeping remote, Targeted, and local service registration in one application
boundary;
providing a standard structure for complex NDNSF applications.
```

`ServiceContainer` is not responsible for:

```text
changing the Request/ACK/Selection/Response wire protocol;
letting a remote caller select a container-local mode;
bypassing remote permissions, signatures, NAC-ABE, UserToken, ProviderToken, or
replay protection;
embedding application-specific state models into NDNSF core.
```

Container registration is intentionally conservative. Applications should
register users, providers, local services, and lifecycle hooks before calling
`start()`. After `start()`, role and hook registration is rejected so the
process-level service boundary does not change while requests are in flight.
`start()` is idempotent; if a start hook throws, hooks that already started are
stopped in reverse order and the container remains stopped. `stop()` is also
idempotent and runs stop hooks in reverse order.

For container-owned local helpers, prefer `container.addLocalService(...)` over
calling `container.localRegistry().registerLocalService(...)` directly. The
registry accessor remains available as a low-level escape hatch and for local
invocation, but `addLocalService(...)` applies the same lifecycle boundary as
role and hook registration. Re-registering the same local service name before
`start()` follows `LocalServiceRegistry` semantics and replaces the previous
handler. After `stop()`, the application may adjust roles, local services, and
lifecycle hooks before starting the container again.

The service invocation APIs remain unchanged:

```cpp
ndn_service_framework::ServiceContainer container({
  ndn::Name("/example/app/container"),
  ndn::Name("/example/group"),
  ndn::Name("/example/controller"),
  "examples/trust-any.conf"
});

container.addUser("operator", user);
container.addProvider("drone-services", provider);
container.addController("controller", controller);

container.provider("drone-services").addHandler<RequestT, ResponseT>(
  serviceName, handler);

container.user("operator").RequestService<RequestT, ResponseT>(
  providers, serviceName, request, onResponse, onTimeout, timeoutMs, strategy);

container.addLocalService<LocalRequest, LocalResponse>(
  localServiceName, localHandler);

container.addLifecycleHook("repo-helper", {
  [] { /* start application helper */ },
  [] { /* stop application helper */ }
});

container.start();
container.stop();
```

In short, `ServiceController` is the network-facing authority/policy role,
`ServiceProvider` is the network-facing provider role, `ServiceUser` is the
network-facing caller role, and `ServiceContainer` is the trusted process-local
runtime that composes and manages those roles.

`RequestT` and `ResponseT` only need protobuf-like methods:

```cpp
bool SerializeToString(std::string* out) const;
bool ParseFromArray(const void* data, size_t size);
```

Application handler callbacks can be moved off the Face/event-loop thread:

```cpp
provider.setHandlerThreads(2);
user.setHandlerThreads(1);
```

The default is `0`, which preserves inline callback execution. With
`handlerThreads > 0`, provider ACK/admission callbacks, provider selected
request execution callbacks, and user response callbacks run on bounded worker
queues. Face, IMS, SVS, NAC-ABE consume/produce publication steps, token checks,
and framework state maps stay on the Face event loop. If `handlerThreads > 1`,
application handlers must be thread-safe or externally synchronized.

The HELLO examples expose the same setting:

```bash
./build/examples/App_Provider --handler-threads 2
./build/examples/App_User --handler-threads 1
```

### 3.2 Unified serviceName rule

Use one unified `serviceName` for the complete endpoint path:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
```

Do not design new code around separate `ServiceName + FunctionName` paths. The split form remains only for legacy compatibility.

### 3.3 V2 naming note

Generic runtime paths use V2 naming helpers with one unified variable-length
`serviceName`. The service name is parsed by position: the trailing `requestId`
is fixed in Request, ACK, Selection, and Response names.
When a user or provider identity appears inside another identity's namespace,
it is encoded as a single URI component so the remaining middle components are
unambiguously the service name.

Request:

```text
/<requester>/NDNSF/REQUEST/<serviceName...>/<requestId>
```

Response:

```text
/<provider>/NDNSF/RESPONSE/<requester-uri-component>/<serviceName...>/<requestId>
```

ACK:

```text
/<provider>/NDNSF/ACK/<requester-uri-component>/<serviceName...>/<requestId>
```

Selection:

```text
/<requester>/NDNSF/SELECTION/<serviceName...>/<requestId>
```

New V2 selection messages use this one unified name shape for both single-provider
and multi-provider selections. The selected provider set is carried in the
`ServiceSelectionMessage` payload as provider entries. Each entry is bound to one
provider identity and contains a provider-bound token proof plus any
provider-specific assignment payload. A provider decrypts the service-level
selection payload, executes only if it finds its own entry, and rejects the
message otherwise. Older provider-specific selection names of the form
`.../SELECTION/<provider-uri-component>/<service>/<requestId>` remain parseable
only as compatibility input for existing regressions and deployments.

### 3.4 Permission model

Permissions are fetched directly from `ServiceController`.

Permission Interest names:

```text
/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity...>
/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity...>
```

Permission discovery Interests are normally unsigned. `ServiceController` parses the target identity from the Interest name, builds a `PermissionResponse` for that target, encrypts it to the target identity certificate, signs the returned Data with the controller identity, and puts the Data. A different identity may fetch another target's encrypted PermissionResponse but cannot decrypt it. User and provider runtimes reject plaintext `PermissionResponse` Data on this path. `PermissionResponse` describes allowed user/provider/service mappings; its legacy token field is deprecated and empty.

The controller does not issue service invocation tokens. Service invocation uses per-request `UserToken` values generated by `ServiceUser` and per-ACK `ProviderToken` values generated by `ServiceProvider`. NAC-ABE attributes enforce group-level access, while the one-time tokens bind ACK, selection, and response messages to a specific request.

This PermissionResponse encryption is not NAC-ABE.

NAC-ABE remains the runtime encryption mechanism for NDNSF service request and response messages, future selection payloads, content keys, IMS, and SVS-backed runtime publication.

Runtime certificate selection separates encryption from signing at startup.
NAC-ABE and PermissionResponse unwrap currently require an RSA-capable identity
certificate, so each user/provider/controller identity must have an RSA
encryption certificate. If the same identity also has an EC/ECDSA certificate,
`ServiceUser`, `ServiceProvider`, and application-local high-frequency Data
publishers cache it as the signing certificate for NDN Data, Interest, and SVS
signing. If no EC signing certificate is installed at startup, that runtime keeps
using the RSA encryption certificate for signing until it is restarted. Existing
constructors perform this compatible selection automatically; explicit
constructors are also available when a deployment wants to pass `encryptionCert`
and `signingCert` separately. The ServiceController/AA path remains RSA-bound
for NAC-ABE public-parameter and DKEY compatibility unless it is separately
validated.

### 3.5 Certificate publishing in distributed deployments

NDN certificates are named Data packets. In a distributed deployment, user,
provider, controller, and AA certificates must be reachable by their certificate
names through routing/FIB, just like service Data. Remote validators, NAC-ABE
authorities, and controllers may fetch these certificates during DKEY,
permission, and bootstrap flows.

NDNSF deployments should use an application root identity as the trust anchor.
Prefer making this root identity the application namespace itself, such as
`/example/hello`, `/example/uav`, or `/example/repo`, rather than adding a
non-parent suffix such as `/root`.
Each node first creates its own identity key, then obtains an NDN certificate
for that key signed by the application root. The node keeps the private key for
its own identity, installs the root certificate as the local trust anchor, and
serves its root-signed certificate by name. The MiniNDN HELLO harness follows
this model by creating `/example/hello` and using it to sign
`/example/hello/controller`, `/example/hello/user`, and provider certificates
before distributing the node keychains.

The framework provides `ndn_service_framework::CertificatePublisher` for this:

```cpp
ndn_service_framework::CertificatePublisher certPublisher(
  face,
  keyChain,
  providerCert.getName());
```

It locates the certificate in the local KeyChain. By default it also serves the
default certificates for the other keys under the same identity, such as an RSA
encryption certificate plus an EC/ECDSA signing certificate. It registers each
served certificate's `.../KEY/<key-id>` prefix and returns only the certificate
whose Data name matches the incoming Interest. The HELLO examples enable this by
default and accept `--no-serve-certificates` for deployments that already serve
certificates through another mechanism.

Manual certificate bootstrap with physical access:

When the operator has physical access to every machine, NDNCERT is optional.
The safest manual flow is to let each node generate its own private key locally;
only the certificate request leaves the node. The CA/root machine signs that
request and returns a public certificate.

On the CA/root machine:

```bash
ndnsec key-gen -t r /ndn/ndnsf/demo > root.cert
ndnsec cert-install -f root.cert
```

On one node, for example drone A:

```bash
ndnsec key-gen -n -t r /ndn/ndnsf/demo/drone/A > drone-A.req
```

Copy `drone-A.req` to the CA/root machine and sign it:

```bash
ndnsec cert-gen -s /ndn/ndnsf/demo -i ROOT drone-A.req > drone-A.cert
```

Copy `root.cert` and `drone-A.cert` back to drone A and install them:

```bash
ndnsec cert-install -f root.cert
ndnsec cert-install -f drone-A.cert
ndnsec-ls-identity -c
```

Do not export/import a safebag in this flow; the node private key never leaves
the node. Use safebags only when a deployment intentionally generates keys on
one machine and transfers private keys to another machine.

### 3.6 Examples

`/examples/generic-dynamic-user-provider.cpp` is the minimal generic dynamic example. It uses `ServiceProvider::addHandler<RequestT, ResponseT>` and `ServiceUser::RequestService<RequestT, ResponseT>` directly, without generated service users, generated service providers, generated services, or stubs. It uses local/mock request publication so it can demonstrate the request/response flow without requiring real NFD/network.

`/examples/ServiceContainer_LocalHelper.cpp` is the minimal ServiceContainer
composition example. One process owns a user role, a provider role, and a
trusted local helper. The provider's remote service handler invokes the local
helper through `LocalServiceRegistry`, while the user still sees only the normal
remote service invocation. This demonstrates that local helpers are internal
composition tools and do not become externally selectable network services.

Build it with:

```bash
./waf configure --with-examples
./waf build --target=generic-dynamic-user-provider
./waf build --target=service-container-local-helper
```

`/examples/App_ServiceController.cpp`, `/examples/App_Provider.cpp`, and `/examples/App_User.cpp` are the current HELLO regression examples. They use controller-issued permission mappings, dynamic `addService(...)`, `RequestMessage.payload = "HELLO"`, `ResponseMessage.payload = "HELLO"`, `AckDecision` metadata payloads, `UserToken`/`ProviderToken` handshakes, and timeout-driven custom selection over `AckSelectionCandidate`.

See `/examples/wscript` for how to compile the examples.

### 3.7 How to run examples

Before running examples on multiple machines, install identity certificates as
described in the manual certificate bootstrap section above. Local regression
scripts create their own temporary keychain material automatically.

The current HELLO examples are exercised by the regression scripts below.

```bash
./examples/run_hello_auth_regression.sh
./examples/run_hello_ack_payload_regression.sh
./examples/run_selective_ack_custom_selection_regression.sh
./examples/run_nac_abe_attribute_routing_regression.sh
./examples/run_token_handshake_negative_regression.sh
```

`run_hello_auth_regression.sh` verifies controller-issued user/provider permission mappings, the generic HELLO request/response flow, and `UserToken` propagation through request, ACK, and response.

`run_hello_ack_payload_regression.sh` verifies that providers publish ACK metadata payloads and users collect the payload before receiving the HELLO response.

`run_selective_ack_custom_selection_regression.sh` verifies multi-provider selective ACK, ACK payload metadata, timeout-driven custom selection, Provider C rejection, Provider B selection, and that only Provider B publishes the final response.

`run_nac_abe_attribute_routing_regression.sh` verifies runtime `GetAttributesByName` logs for NAC-ABE routing: REQUEST and SELECTION use `/SERVICE/HELLO`, while ACK and RESPONSE use `/PERMISSION/HELLO`.

`run_token_handshake_negative_regression.sh` verifies rejection of ACKs and responses with wrong `UserToken` values, selection messages with wrong `ProviderToken` values, and replayed ProviderTokens.

### 3.8 Python wrapper and higher-level application packages

The Python wrapper is a binding to the current C++ runtime, not a separate
framework implementation. It supports ordinary service handlers,
service-independent ACK decisions, application-defined ACK selection,
asynchronous requests, collaboration handlers, collaboration requests,
encrypted large-data publication, standard large-data reference payloads, and
NDN segmented Data helpers. See `pythonWrapper/README.md` for API examples.

`NDNSF-DistributedInference` builds on this wrapper and exposes model-plan and
distributed inference APIs. Its current path supports ONNX chunk policies,
dependency-driven execution, repo-backed or NDN-backed artifacts, activation
exchange through large-data references, and MiniNDN smoke tests. It remains an
application package above NDNSF core; model-specific splitters and planners are
kept there rather than inside the framework core.

`NDNSF-DistributedRepo` is a repository-oriented application layer. It should
store and serve application-published NDN Data segments or references without
redefining NDNSF service security. Repo details are intentionally outside the
core service invocation protocol.

`NDNSF-UAV-APP` is a UAV service application over NDNSF. Cross-node control,
telemetry, video, recording discovery, and mission operations use NDNSF
remote/Targeted services. Same-process helpers may use
`ServiceContainer::localRegistry()` through `container.addLocalService(...)`,
but local helpers are not externally selectable services.

### 3.9 Security-mechanism alignment for these regressions

```text
Permission distribution:
  User fetches /NDNSF/PERMISSIONS/USER/<user>.
  Provider fetches /NDNSF/PERMISSIONS/PROVIDER/<provider>.
  Permission discovery Interests are normally unsigned.
  ServiceController signs the returned Data with the controller identity.
  PermissionResponse payloads are encrypted to the target identity certificate.

NAC-ABE attributes:
  REQUEST and SELECTION use /SERVICE/<service>.
  ACK and RESPONSE use /PERMISSION/<service>.

Authorization:
  User requests carry a one-time UserToken generated by ServiceUser.
  ACKs carry the same UserToken and a one-time ProviderToken generated by ServiceProvider.
  V2 Selection messages carry provider-specific entries. New unified V2
  selections do not expose plaintext ProviderTokens in the shared service-level
  payload; each entry carries a provider-bound ProviderToken proof instead.
  Responses carry the original UserToken.
  Users reject ACK/response UserToken mismatches.
  Providers reject selection ProviderToken mismatches.
  Providers reject replayed ProviderTokens for consumed or new request IDs.
  Targeted fast-path requests carry one unused ProviderToken obtained from a
  prior targeted bootstrap/refill response, and targeted responses echo the
  paired UserToken.
  Targeted services refill token batches through the normal ACK/Selection path.
  Providers must install their own provider permission before serving a service.
  Service authorization is enforced by NAC-ABE attributes, provider permission checks, and token handshake validation.
```

### 3.10 How to log to file
For example, assuming your program is `./app` and you want to log everything, first set the log level in the command line using:

```bash
export NDN_LOG="*=TRACE"
```

Then run:

```bash
./app > filename.log 2>&1
```

The output will be saved in the file `filename.log` in the current directory.
If you're using MiniNDN, the output will be stored under `/tmp/minindn/<nodeName>`.

### 3.11 MiniNDN latency reproduction profile

The low-latency HELLO benchmark uses the dynamic API with one Memphis user, one
UCLA provider, the CSU controller, no adaptive admission control, SVS maximum
suppression set to 1 ms, and performance-mode logging. Hot-path per-message logs
must stay below `INFO`; otherwise request/ACK/selection/response logging can
dominate the benchmark.

For performance runs, keep output intentionally sparse. The code treats
per-request, per-ACK/selection/response, lifecycle, publication, and detailed
diagnostic events as `TRACE` logs. Use `NDN_LOG=ndn_service_framework.*=INFO`
for normal performance measurements, or `DEBUG` when checking startup/state
without hot-path traces. Use `TRACE` only for performance analysis or debugging.
Performance-analysis traces are sampled by default: `--timeline-trace` enables
timeline/lifecycle diagnostics, and `--timeline-trace-sample-rate N` keeps one
stable request sample out of every `N` request IDs (`100` by default). Use
`--timeline-trace-sample-rate 1` only for very short focused debugging. Likewise,
keep NFD packet dumps and other detailed diagnostics disabled unless the run is
specifically investigating a bottleneck. Long 10-minute rate runs can generate
enough raw output to perturb latency and fill the filesystem; keep only summary
artifacts after the useful finding has been recorded.

Key runtime settings:

```text
NDN_LOG=ndn_service_framework.*=INFO
NFD log level: WARN
NDNSF_SVS_MAX_SUPPRESSION_MS=1
NDNSF_SVS_ASYNC_PUBLISH=1
NDNSF_SVS_PARALLEL_SYNC=1
NDNSF_SVS_PARALLEL_WORKERS=4
NDNSF_SVS_PARALLEL_QUEUE=256
NDNSF_SVS_PARALLEL_PRODUCTION=4
NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING=0
NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK=1
adaptive admission: disabled
provider handler threads: 2
provider ACK worker threads: 2
strategy: first-responding
workload: open-loop, 60 s warmup + 60 s measured duration for latency floor validation
```

Verified software stack for the 100 RPS latency-floor run:

```text
OS: Ubuntu 20.04.3 LTS
Compiler: g++ 9.4.0
Python: 3.8.10
Boost: 1.71.0
OpenSSL: 1.1.1f
ndn-cxx: 0.9.0 (/usr/local/lib/libndn-cxx.so.0.9.0)
NFD: 24.07-14-g2b43d675
MiniNDN: 0.7.0 (/home/tianxing/NDN/mini-ndn)
Mininet: 2.3.1b4
ndn-svs: /home/tianxing/NDN/ndn-svs commit 70302b6
NDNSF: this repository commit that records the profile
OpenABE: /usr/local/lib/libopenabe.so, built against OpenSSL 1.1.x
```

The exact commit hashes are part of the reproduction record, not a permanent
minimum version claim. If the latency floor regresses after updating any of
`ndn-cxx`, `NFD`, `ndn-svs`, MiniNDN/Mininet, OpenABE, or this repository, rerun
the command below before comparing against the 166 ms reference. Also verify
that the application links against the intended `/usr/local/lib/libndn-svs.so`
and `/usr/local/lib/libndn-cxx.so`; stale system libraries can make source
changes appear ineffective.

Reproduction command:

```bash
sudo -n python3 Experiments/NDNSF_NewAPI_Minindn_Perf.py \
  --topology-file 'Experiments/Topology/testbed(loss=0%).conf' \
  --user-node memphis \
  --provider-nodes ucla \
  --controller-node csu \
  --providers 1 \
  --rate-rps 100 \
  --duration 60 \
  --warmup 60 \
  --max-total-runtime-seconds 300 \
  --workload-mode open-loop \
  --strategy first-responding \
  --disable-adaptive-admission-control \
  --performance-mode \
  --handler-threads 2 \
  --ack-threads 2 \
  --nfd-log-level WARN \
  --skip-post-run-diagnostics
```

Reference result from `results/newapi_testbed_rate_series_20260528_194238`:

```text
RPS   Actual   Success   Avg ms   P50 ms   P95 ms   P99 ms   Timeout
20    20.00    100%      166.19   165.20   172.88   178.70   0
60    60.00    100%      168.85   166.61   184.34   199.18   0
100   99.99    100%      166.40   165.67   169.04   174.19   0
```

Current 60-second diagnostics with the StateVectorSync v3 bootstrap-time ndn-svs
build confirm that the low-latency band is still reachable after switching the
SVS wire format to `StateVectorEntry(Name, SeqNoEntry(BootstrapTime, SeqNo))`.
The key harness setting is `NDNSF_SVS_PARALLEL_PRODUCTION=4`. A previous
`--performance-mode` default accidentally forced this value to `0`, which pushed
the same 20 RPS workload to about 210 ms. Restoring the performance profile to
parallel Sync Interest production brought both 20 RPS and 100 RPS back to the
160-170 ms band:

```text
Result directory                                      RPS    Actual   Success   Avg ms   P50 ms   P95 ms   P99 ms   Timeout
results/newapi_testbed_rate_series_20260529_201154   20     20.00    100%      162.04   161.92   163.68   165.83   0
results/newapi_testbed_rate_series_20260529_201458   100    100.00   100%      164.38   164.00   167.46   173.67   0
```

Do not change ndn-svs periodic Sync Interest timing for this benchmark unless
the experiment explicitly studies that timer: periodic sync affects piggyback
opportunities. Keep the Sync Interest suppression interval in the 1-5 ms range;
the reproduction profile uses 1 ms.

The current open-loop generator avoids zero-delay catch-up bursts. When the
event loop falls behind, it records delayed publications and uses bounded
catch-up spacing instead of publishing multiple due requests back-to-back. In a
60-second 100 RPS run (`results/newapi_minindn_perf_20260529_125158`), the
generator kept 99.995 actual RPS while reducing sub-1 ms send gaps to zero:

```text
Actual RPS  Success  Avg ms  P50 ms  P95 ms  P99 ms  Timeout
99.995      99.95%   203.81  201.47  235.27  251.08  3
```

A sampled timeline run (`results/newapi_minindn_perf_20260529_125523`) showed
that AES-GCM encryption and local publish calls are not the bottleneck
(publish/crypto p50 is sub-millisecond). The remaining latency is in SVS/NFD
delivery: REQUEST-to-ACK p50 is about 97 ms, and SELECTION-to-RESPONSE p50 is
about 96 ms. The four one-way user/provider delivery legs are each about
46-50 ms, while the Memphis-to-UCLA route cost is 37 ms. Future optimization
should therefore focus on the SVS/NFD delivery path and piggyback delivery
effectiveness, not on periodic-sync timer changes, extra hot-path logging, or
application crypto.

The main lesson from the 2026-05-29 regression is that a benchmark can look slow
even when the protocol and code path are correct if the harness silently disables
parallel SVS production. Keep `--performance-mode` aligned with the runtime
profile above when validating the latency floor, and only set
`--svs-disable-parallel-production` when an experiment is specifically studying
single-threaded production behavior.
