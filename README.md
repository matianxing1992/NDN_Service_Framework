# ndn_service_framework

1. Prerequisites  
To ensure version consistency, please install according to the corresponding libraries from the specified directory.  
ndn-cxx: https://github.com/named-data/ndn-cxx/  
NDNSD: https://github.com/matianxing1992/NDNSD  
ndn-svs: https://github.com/matianxing1992/ndn-svs  
NAC-ABE: https://github.com/matianxing1992/NAC-ABE  

2. Installation  
./waf configure  
sudo ./waf build  
sudo ./waf install  

3. How-to

3.1 Generic dynamic API, preferred for new applications

New applications should use the framework-core generic dynamic API directly. This path does not require generated `ServiceUser_*`, `ServiceProvider_*`, `*Service`, or `*ServiceStub` classes.

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

user.asyncCall<ObjectDetectionRequest, ObjectDetectionResponse>(
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

`RequestT` and `ResponseT` only need protobuf-like methods:

```cpp
bool SerializeToString(std::string* out) const;
bool ParseFromArray(const void* data, size_t size);
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

Generic runtime paths use V2 naming helpers. V2 names carry explicit component counts and the unified variable-length `serviceName`.

Request:

```text
/<requester>/NDNSF/REQUEST/<serviceComponentCount>/<serviceName...>/<bloomFilter>/<requestId>
```

Response:

```text
/<provider>/NDNSF/RESPONSE/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
```

ACK:

```text
/<provider>/NDNSF/ACK/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
```

3.4 Permission model

Permissions are fetched directly from `ServiceController`.

Permission Interest names:

```text
/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity...>
/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity...>
```

`ServiceController` returns a `PermissionResponse`. The preferred permission response path is encrypted for the target certificate using the PermissionResponse encryption helpers. This PermissionResponse encryption is not NAC-ABE.

NAC-ABE remains the runtime encryption mechanism for NDNSF service request and response messages, future coordination payloads, content keys, IMS, and SVS-backed runtime publication.

3.5 Example:

`/examples/generic-dynamic-user-provider.cpp` is the minimal generic dynamic example. It uses `ServiceProvider::addHandler<RequestT, ResponseT>` and `ServiceUser::asyncCall<RequestT, ResponseT>` directly, without generated service users, generated service providers, generated services, or stubs. It uses local/mock request publication so it can demonstrate the request/response flow without requiring real NFD/network.

Build it with:

```bash
./waf configure --with-examples
./waf build --target=generic-dynamic-user-provider
```

`/examples/App_ServiceController.cpp`, `/examples/App_Provider.cpp`, and `/examples/App_User.cpp` are the current HELLO regression examples. They use controller-issued permissions, dynamic `addService(...)`, `RequestMessage.payload = "HELLO"`, `ResponseMessage.payload = "HELLO"`, `AckDecision` metadata payloads, and timeout-driven custom selection over `AckSelectionCandidate`.

See `/examples/wscript` for how to compile the examples.

3.6 Legacy CodeGenerator compatibility

The `/CodeGenerator` directory contains the legacy generator: `Generated`, `Template`, `app.yml`, and `NDNSFCodeGenerator.py`.

The `app.yml` file defines services and message types. Running `sudo python NDNSFCodeGenerator.py` generates compatibility files in the `Generated` folder based on the templates in `Template`.

Generated classes such as `ServiceUser_*`, `ServiceProvider_*`, `*Service`, and `*ServiceStub` are legacy compatibility code only. They should not be treated as the primary architecture for new applications.

Generated C++ outputs are not checked in unless an active build target or compatibility test intentionally uses them.

3.7 How to run examples:

NDN requires creating a corresponding root certificate, then using the root certificate to generate the corresponding sub-certificates. Both the root certificate and these sub-certificates need to be installed on each node. `/Experiments/NDNSFExperiment_AutoConfig.py` provides an example of how to create certificates, which you need to modify according to your own requirements.

The current HELLO examples are exercised by `examples/run_hello_auth_regression.sh` and `examples/run_hello_ack_payload_regression.sh`.

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

