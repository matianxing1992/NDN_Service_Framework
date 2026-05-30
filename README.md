# ndn_service_framework

1. Prerequisites

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

2. Installation

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

OpenABE and OpenSSL note:

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

3. How-to

3.1 Generic dynamic API, preferred for new applications

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
  providers,
  ndn::Name("/ObjectDetection/YOLOv8"),
  request,
  [](const ObjectDetectionResponse& response) {
    // Handle typed response.
  },
  [] {
    // Handle timeout.
  },
  1000,
  ndn_service_framework::tlv::FirstResponding);
```

For known-provider low-latency commands, such as UAV flight-control/MAVLink
execution, use targeted invocation. Targeted invocation still uses NDNSF
`RequestMessage`/`ResponseMessage`, signing, permission checks, one-time token
checks, and replay protection, but skips the normal ACK/Selection phase:

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

`RequestServiceDirect(...)` and `addDirectService(...)` remain compatibility
aliases, but new code should use the more precise `Targeted` terminology.

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

3.2 Unified serviceName rule

Use one unified `serviceName` for the complete endpoint path:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
```

Do not design new code around separate `ServiceName + FunctionName` paths. The split form remains only for legacy compatibility.

3.3 V2 naming note

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
/<requester>/NDNSF/SELECTION/<provider-uri-component>/<serviceName...>/<requestId>
```

3.4 Permission model

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

3.5 Certificate publishing in distributed deployments

NDN certificates are named Data packets. In a distributed deployment, user,
provider, controller, and AA certificates must be reachable by their certificate
names through routing/FIB, just like service Data. Remote validators, NAC-ABE
authorities, and controllers may fetch these certificates during DKEY,
permission, and bootstrap flows.

NDNSF deployments should use an application root identity as the trust anchor.
Each node first creates its own identity key, then obtains an NDN certificate
for that key signed by the application root. The node keeps the private key for
its own identity, installs the root certificate as the local trust anchor, and
serves its root-signed certificate by name. The MiniNDN HELLO harness follows
this model by creating `/example/hello/root` and using it to sign the
controller, user, and provider certificates before distributing the node
keychains.

The framework provides `ndn_service_framework::CertificatePublisher` for this:

```cpp
ndn_service_framework::CertificatePublisher certPublisher(
  face,
  keyChain,
  providerCert.getName());
```

It locates the certificate in the local KeyChain, serves the existing certificate
Data under its exact certificate name, and by default registers the certificate's
`.../KEY/<key-id>` prefix. The HELLO examples enable this by default and accept
`--no-serve-certificates` for deployments that already serve certificates
through another mechanism.

3.6 Example:

`/examples/generic-dynamic-user-provider.cpp` is the minimal generic dynamic example. It uses `ServiceProvider::addHandler<RequestT, ResponseT>` and `ServiceUser::RequestService<RequestT, ResponseT>` directly, without generated service users, generated service providers, generated services, or stubs. It uses local/mock request publication so it can demonstrate the request/response flow without requiring real NFD/network.

Build it with:

```bash
./waf configure --with-examples
./waf build --target=generic-dynamic-user-provider
```

`/examples/App_ServiceController.cpp`, `/examples/App_Provider.cpp`, and `/examples/App_User.cpp` are the current HELLO regression examples. They use controller-issued permission mappings, dynamic `addService(...)`, `RequestMessage.payload = "HELLO"`, `ResponseMessage.payload = "HELLO"`, `AckDecision` metadata payloads, `UserToken`/`ProviderToken` handshakes, and timeout-driven custom selection over `AckSelectionCandidate`.

See `/examples/wscript` for how to compile the examples.

3.6 How to run examples:

NDN requires creating a corresponding root certificate, then using the root certificate to generate the corresponding sub-certificates. Both the root certificate and these sub-certificates need to be installed on each node. `/Experiments/NDNSFExperiment_AutoConfig.py` provides an example of how to create certificates, which you need to modify according to your own requirements.

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

Security-mechanism alignment for these regressions:

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
  Selection messages carry the selected provider's ProviderToken.
  Responses carry the original UserToken.
  Users reject ACK/response UserToken mismatches.
  Providers reject selection ProviderToken mismatches.
  Providers reject replayed ProviderTokens for consumed or new request IDs.
  Providers must install their own provider permission before serving a service.
  Service authorization is enforced by NAC-ABE attributes, provider permission checks, and token handshake validation.
```

3.8 How to log to file:
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

3.9 MiniNDN latency reproduction profile

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
